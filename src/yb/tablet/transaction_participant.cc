//
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//

#include "yb/tablet/transaction_participant.h"

#include <mutex>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>

#include <boost/optional/optional.hpp>

#include <boost/uuid/uuid_io.hpp>

#include "yb/rocksdb/write_batch.h"

#include "yb/client/transaction_rpc.h"

#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/key_bytes.h"

#include "yb/rpc/rpc.h"

#include "yb/tserver/tserver_service.pb.h"

#include "yb/util/locks.h"
#include "yb/util/monotime.h"

using namespace std::placeholders;

namespace yb {
namespace tablet {

namespace {

class RunningTransaction {
 public:
  explicit RunningTransaction(TransactionMetadata metadata, rpc::Rpcs* rpcs)
      : metadata_(std::move(metadata)), rpcs_(*rpcs),
        get_status_handle_(rpcs->InvalidHandle()),
        abort_handle_(rpcs->InvalidHandle()) {
  }

  ~RunningTransaction() {
    rpcs_.Abort({&get_status_handle_, &abort_handle_});
  }

  const TransactionId& id() const {
    return metadata_.transaction_id;
  }

  const TransactionMetadata& metadata() const {
    return metadata_;
  }

  HybridTime local_commit_time() const {
    return local_commit_time_;
  }

  void SetLocalCommitTime(HybridTime time) {
    local_commit_time_ = time;
  }

  void RequestStatusAt(client::YBClient* client,
                       HybridTime time,
                       TransactionStatusCallback callback,
                       std::unique_lock<std::mutex>* lock) const {
    if (last_known_status_hybrid_time_ > HybridTime::kMin) {
      auto transaction_status =
          GetStatusAt(time, last_known_status_hybrid_time_, last_known_status_);
      if (transaction_status) {
        lock->unlock();
        callback(TransactionStatusResult{*transaction_status, last_known_status_hybrid_time_});
        return;
      }
    }
    bool was_empty = status_waiters_.empty();
    status_waiters_.push_back(StatusWaiter{std::move(callback), time});
    if (!was_empty) {
      return;
    }
    lock->unlock();
    auto deadline = MonoTime::FineNow() + MonoDelta::FromSeconds(5); // TODO(dtxn)
    tserver::GetTransactionStatusRequestPB req;
    req.set_tablet_id(metadata_.status_tablet);
    req.set_transaction_id(metadata_.transaction_id.begin(), metadata_.transaction_id.size());
    rpcs_.RegisterAndStart(
        client::GetTransactionStatus(
            deadline,
            nullptr /* tablet */,
            client,
            &req,
            std::bind(&RunningTransaction::StatusReceived, this, _1, _2, lock->mutex())),
        &get_status_handle_);
  }

  void Abort(client::YBClient* client,
             TransactionStatusCallback callback,
             std::unique_lock<std::mutex>* lock) const {
    bool was_empty = abort_waiters_.empty();
    abort_waiters_.push_back(std::move(callback));
    lock->unlock();
    if (!was_empty) {
      return;
    }
    auto deadline = MonoTime::FineNow() + MonoDelta::FromSeconds(5); // TODO(dtxn)
    tserver::AbortTransactionRequestPB req;
    req.set_tablet_id(metadata_.status_tablet);
    req.set_transaction_id(metadata_.transaction_id.begin(), metadata_.transaction_id.size());
    rpcs_.RegisterAndStart(
        client::AbortTransaction(
            deadline,
            nullptr /* tablet */,
            client,
            &req,
            std::bind(&RunningTransaction::AbortReceived,
                      this,
                      _1,
                      _2,
                      lock->mutex())),
        &abort_handle_);
  }

 private:
  static boost::optional<tserver::TransactionStatus> GetStatusAt(
      HybridTime time,
      HybridTime last_known_status_hybrid_time,
      tserver::TransactionStatus last_known_status) {
    switch (last_known_status) {
      case tserver::TransactionStatus::ABORTED:
        return tserver::TransactionStatus::ABORTED;
      case tserver::TransactionStatus::COMMITTED:
        // TODO(dtxn) clock skew
        return last_known_status_hybrid_time > time
            ? tserver::TransactionStatus::PENDING
            : tserver::TransactionStatus::COMMITTED;
      case tserver::TransactionStatus::PENDING:
        if (last_known_status_hybrid_time >= time) {
          return tserver::TransactionStatus::PENDING;
        }
        return boost::none;
      default:
        FATAL_INVALID_ENUM_VALUE(tserver::TransactionStatus, last_known_status);
    }
  }

  void StatusReceived(const Status& status,
                      const tserver::GetTransactionStatusResponsePB& response,
                      std::mutex* mutex) const {
    rpcs_.Unregister(&get_status_handle_);
    decltype(status_waiters_) status_waiters;
    HybridTime time;
    tserver::TransactionStatus transaction_status;
    {
      std::unique_lock<std::mutex> lock(*mutex);
      status_waiters_.swap(status_waiters);
      if (status.ok()) {
        DCHECK(response.has_status_hybrid_time() ||
               response.status() == tserver::TransactionStatus::ABORTED);
        time = response.has_status_hybrid_time()
            ? HybridTime(response.status_hybrid_time())
            : HybridTime::kMax;
        if (last_known_status_hybrid_time_ <= time) {
          last_known_status_hybrid_time_ = time;
          last_known_status_ = response.status();
        }
        time = last_known_status_hybrid_time_;
        transaction_status = last_known_status_;
      }
    }
    if (!status.ok()) {
      for (const auto& waiter : status_waiters) {
        waiter.callback(status);
      }
    } else {
      for (const auto& waiter : status_waiters) {
        auto status_for_waiter = GetStatusAt(waiter.time, time, transaction_status);
        if (status_for_waiter) {
          waiter.callback(TransactionStatusResult{*status_for_waiter, time});
        } else {
          waiter.callback(STATUS_FORMAT(
              TryAgain,
              "Cannot determine transaction status at $0, last known: $1 at $2",
              waiter.time,
              transaction_status,
              time));
        }
      }
    }
  }

  static Result<TransactionStatusResult> MakeAbortResult(
      const Status& status,
      const tserver::AbortTransactionResponsePB& response) {
    if (!status.ok()) {
      return status;
    }

    HybridTime status_time = response.has_status_hybrid_time()
         ? HybridTime(response.status_hybrid_time())
         : HybridTime::kInvalidHybridTime;
    return TransactionStatusResult{response.status(), status_time};
  }

  void AbortReceived(const Status& status,
                     const tserver::AbortTransactionResponsePB& response,
                     std::mutex* mutex) const {
    decltype(abort_waiters_) abort_waiters;
    {
      std::lock_guard<std::mutex> lock(*mutex);
      rpcs_.Unregister(&abort_handle_);
      abort_waiters_.swap(abort_waiters);
    }
    auto result = MakeAbortResult(status, response);
    for (const auto& waiter : abort_waiters) {
      waiter(result);
    }
  }

  TransactionMetadata metadata_;
  rpc::Rpcs& rpcs_;
  HybridTime local_commit_time_ = HybridTime::kInvalidHybridTime;

  struct StatusWaiter {
    TransactionStatusCallback callback;
    HybridTime time;
  };

  mutable tserver::TransactionStatus last_known_status_;
  mutable HybridTime last_known_status_hybrid_time_ = HybridTime::kMin;
  mutable std::vector<StatusWaiter> status_waiters_;
  mutable rpc::Rpcs::Handle get_status_handle_;
  mutable rpc::Rpcs::Handle abort_handle_;
  mutable std::vector<TransactionStatusCallback> abort_waiters_;
};

} // namespace

Result<TransactionMetadata> TransactionMetadata::FromPB(const TransactionMetadataPB& source) {
  TransactionMetadata result;
  auto id = DecodeTransactionId(source.transaction_id());
  RETURN_NOT_OK(id);
  result.transaction_id = *id;
  result.isolation = source.isolation();
  result.status_tablet = source.status_tablet();
  result.priority = source.priority();
  result.start_time = HybridTime(source.start_hybrid_time());
  return result;
}

bool operator==(const TransactionMetadata& lhs, const TransactionMetadata& rhs) {
  return lhs.transaction_id == rhs.transaction_id &&
         lhs.isolation == rhs.isolation &&
         lhs.status_tablet == rhs.status_tablet &&
         lhs.priority == rhs.priority &&
         lhs.start_time == rhs.start_time;
}

std::ostream& operator<<(std::ostream& out, const TransactionMetadata& metadata) {
  return out << Format("{ transaction_id: $0 isolation: $1 status_tablet: $2 priority: $3 "
                           "start_time: $4",
                       metadata.transaction_id, IsolationLevel_Name(metadata.isolation),
                       metadata.status_tablet, metadata.priority, metadata.start_time);
}

class TransactionParticipant::Impl {
 public:
  explicit Impl(TransactionParticipantContext* context)
      : context_(*context) {}

  ~Impl() {
    transactions_.clear();
    rpcs_.Shutdown();
  }

  // Adds new running transaction.
  void Add(const TransactionMetadataPB& data, rocksdb::WriteBatch *write_batch) {
    auto metadata = TransactionMetadata::FromPB(data);
    if (!metadata.ok()) {
      LOG(DFATAL) << "Invalid transaction id: " << metadata.status().ToString();
      return;
    }
    bool store = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = transactions_.find(metadata->transaction_id);
      if (it == transactions_.end()) {
        transactions_.emplace(*metadata, &rpcs_);
        store = true;
      } else {
        DCHECK_EQ(it->metadata(), *metadata);
      }
    }
    if (store) {
      // TODO(dtxn) Load value if it is not loaded.
      docdb::KeyBytes key;
      AppendTransactionKeyPrefix(metadata->transaction_id, &key);
      auto value = data.SerializeAsString();
      write_batch->Put(key.data(), value);
    }
  }

  HybridTime LocalCommitTime(const TransactionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = transactions_.find(id);
    if (it == transactions_.end()) {
      return HybridTime::kInvalidHybridTime;
    }
    return it->local_commit_time();
  }

  boost::optional<TransactionMetadata> Metadata(rocksdb::DB* db, const TransactionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = FindOrLoad(db, id);
    if (it == transactions_.end()) {
      return boost::none;
    }
    return it->metadata();
  }

  void RequestStatusAt(const TransactionId& id,
                       HybridTime time,
                       TransactionStatusCallback callback) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = transactions_.find(id);
    if (it == transactions_.end()) {
      lock.unlock();
      callback(STATUS_FORMAT(NotFound, "Unknown transaction: $1", id));
      return;
    }
    return it->RequestStatusAt(client(), time, std::move(callback), &lock);
  }

  void Abort(const TransactionId& id,
             TransactionStatusCallback callback) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = transactions_.find(id);
    if (it == transactions_.end()) {
      lock.unlock();
      callback(STATUS_FORMAT(NotFound, "Unknown transaction: $1", id));
      return;
    }
    return it->Abort(client(), std::move(callback), &lock);
  }

  CHECKED_STATUS ProcessApply(const TransactionApplyData& data) {
    CHECK_OK(data.applier->ApplyIntents(data));

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = transactions_.find(data.transaction_id);
      if (it == transactions_.end()) {
        // This situation is normal and could be caused by 2 scenarios:
        // 1) Write batch failed, but originator doesn't know that.
        // 2) Failed to notify status tablet that we applied transaction.
        LOG(WARNING) << "Apply of unknown transaction: " << data.transaction_id;
        return Status::OK();
      } else {
        transactions_.modify(it, [&data](RunningTransaction& transaction) {
          transaction.SetLocalCommitTime(data.commit_time);
        });
        // TODO(dtxn) cleanup
      }
      if (data.mode == ProcessingMode::LEADER) {
        auto deadline = MonoTime::FineNow() + MonoDelta::FromSeconds(5); // TODO(dtxn)
        tserver::UpdateTransactionRequestPB req;
        req.set_tablet_id(data.status_tablet);
        auto& state = *req.mutable_state();
        state.set_transaction_id(data.transaction_id.begin(), data.transaction_id.size());
        state.set_status(tserver::TransactionStatus::APPLIED_IN_ONE_OF_INVOLVED_TABLETS);
        state.add_tablets(context_.tablet_id());

        auto handle = rpcs_.Prepare();
        *handle = UpdateTransaction(
            deadline,
            nullptr /* remote_tablet */,
            client(),
            &req,
            [this, handle](const Status& status) {
              rpcs_.Unregister(handle);
              LOG_IF(WARNING, !status.ok()) << "Failed to send applied: " << status.ToString();
            });
        (**handle).SendRpc();
      }
    }
    return Status::OK();
  }

 private:
  typedef boost::multi_index_container<RunningTransaction,
      boost::multi_index::indexed_by <
          boost::multi_index::hashed_unique <
              boost::multi_index::const_mem_fun<RunningTransaction,
                                                const TransactionId&,
                                                &RunningTransaction::id>
          >
      >
  > Transactions;

  Transactions::const_iterator FindOrLoad(rocksdb::DB* db, const TransactionId& id) {
    auto it = transactions_.find(id);
    if (it != transactions_.end()) {
      return it;
    }

    docdb::KeyBytes key;
    AppendTransactionKeyPrefix(id, &key);
    auto iter = docdb::CreateRocksDBIterator(db,
                                             docdb::BloomFilterMode::DONT_USE_BLOOM_FILTER,
                                             boost::none,
                                             rocksdb::kDefaultQueryId);
    iter->Seek(key.data());
    if (iter->Valid() && iter->key() == key.data()) {
      TransactionMetadataPB metadata_pb;
      if (metadata_pb.ParseFromArray(iter->value().cdata(), iter->value().size())) {
        auto metadata = TransactionMetadata::FromPB(metadata_pb);
        if (metadata.ok()) {
          it = transactions_.emplace(std::move(*metadata), &rpcs_).first;
        } else {
          LOG(DFATAL) << "Loaded bad metadata: " << metadata.status();
        }
      } else {
        LOG(DFATAL) << "Unable to parse stored metadata: " << iter->value().ToDebugHexString();
      }
    }

    return it;
  }

  client::YBClient* client() const {
    return context_.client_future().get().get();
  }

  TransactionParticipantContext& context_;

  std::mutex mutex_;
  rpc::Rpcs rpcs_;
  Transactions transactions_;
};

TransactionParticipant::TransactionParticipant(TransactionParticipantContext* context)
    : impl_(new Impl(context)) {
}

TransactionParticipant::~TransactionParticipant() {
}

void TransactionParticipant::Add(const TransactionMetadataPB& data,
                                 rocksdb::WriteBatch *write_batch) {
  impl_->Add(data, write_batch);
}

boost::optional<TransactionMetadata> TransactionParticipant::Metadata(
    rocksdb::DB* db, const TransactionId& id) {
  return impl_->Metadata(db, id);
}

HybridTime TransactionParticipant::LocalCommitTime(const TransactionId& id) {
  return impl_->LocalCommitTime(id);
}

void TransactionParticipant::RequestStatusAt(const TransactionId& id,
                                             HybridTime time,
                                             TransactionStatusCallback callback) {
  return impl_->RequestStatusAt(id, time, std::move(callback));
}

void TransactionParticipant::Abort(const TransactionId& id,
                                   TransactionStatusCallback callback) {
  return impl_->Abort(id, std::move(callback));
}

CHECKED_STATUS TransactionParticipant::ProcessApply(const TransactionApplyData& data) {
  return impl_->ProcessApply(data);
}

void AppendTransactionKeyPrefix(const TransactionId& transaction_id, docdb::KeyBytes* out) {
  out->AppendValueType(docdb::ValueType::kIntentPrefix);
  out->AppendValueType(docdb::ValueType::kTransactionId);
  out->AppendRawBytes(Slice(transaction_id.data, transaction_id.size()));
}

} // namespace tablet
} // namespace yb
