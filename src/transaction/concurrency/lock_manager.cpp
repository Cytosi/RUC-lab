/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

bool LockManager::is_compatible(LockMode requested, LockMode granted) const {
    switch (requested) {
        case LockMode::SHARED:
            return granted == LockMode::SHARED || granted == LockMode::INTENTION_SHARED;
        case LockMode::EXLUCSIVE:
            return false;
        case LockMode::INTENTION_SHARED:
            return granted != LockMode::EXLUCSIVE;
        case LockMode::INTENTION_EXCLUSIVE:
            return granted == LockMode::INTENTION_SHARED || granted == LockMode::INTENTION_EXCLUSIVE;
        case LockMode::S_IX:
            return granted == LockMode::INTENTION_SHARED;
    }
    return false;
}

bool LockManager::is_same_or_stronger(LockMode held, LockMode requested) const {
    if (held == requested || held == LockMode::EXLUCSIVE) {
        return true;
    }
    if (held == LockMode::S_IX) {
        return requested == LockMode::SHARED || requested == LockMode::INTENTION_SHARED ||
               requested == LockMode::INTENTION_EXCLUSIVE;
    }
    if (held == LockMode::SHARED) {
        return requested == LockMode::INTENTION_SHARED;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE) {
        return requested == LockMode::INTENTION_SHARED;
    }
    return false;
}

void LockManager::refresh_group_lock_mode(LockRequestQueue& request_queue) {
    bool has_is = false;
    bool has_ix = false;
    bool has_s = false;
    bool has_x = false;
    bool has_six = false;

    for (auto& request : request_queue.request_queue_) {
        if (!request.granted_) {
            continue;
        }
        switch (request.lock_mode_) {
            case LockMode::INTENTION_SHARED:
                has_is = true;
                break;
            case LockMode::INTENTION_EXCLUSIVE:
                has_ix = true;
                break;
            case LockMode::SHARED:
                has_s = true;
                break;
            case LockMode::EXLUCSIVE:
                has_x = true;
                break;
            case LockMode::S_IX:
                has_six = true;
                break;
        }
    }

    if (has_x) {
        request_queue.group_lock_mode_ = GroupLockMode::X;
    } else if (has_six) {
        request_queue.group_lock_mode_ = GroupLockMode::SIX;
    } else if (has_s) {
        request_queue.group_lock_mode_ = GroupLockMode::S;
    } else if (has_ix) {
        request_queue.group_lock_mode_ = GroupLockMode::IX;
    } else if (has_is) {
        request_queue.group_lock_mode_ = GroupLockMode::IS;
    } else {
        request_queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    }
}

bool LockManager::lock_on_data(Transaction* txn, const LockDataId& lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) {
        return true;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        txn->set_state(TransactionState::ABORTED);
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    std::scoped_lock lock(latch_);
    auto& request_queue = lock_table_[lock_data_id];

    for (auto it = request_queue.request_queue_.begin(); it != request_queue.request_queue_.end(); ++it) {
        if (it->txn_id_ != txn->get_transaction_id()) {
            continue;
        }
        if (is_same_or_stronger(it->lock_mode_, lock_mode)) {
            it->granted_ = true;
            txn->get_lock_set()->insert(lock_data_id);
            refresh_group_lock_mode(request_queue);
            return true;
        }

        for (auto& request : request_queue.request_queue_) {
            if (request.txn_id_ == txn->get_transaction_id() || !request.granted_) {
                continue;
            }
            if (!is_compatible(lock_mode, request.lock_mode_) || !is_compatible(request.lock_mode_, lock_mode)) {
                txn->set_state(TransactionState::ABORTED);
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT);
            }
        }

        it->lock_mode_ = lock_mode;
        it->granted_ = true;
        txn->get_lock_set()->insert(lock_data_id);
        refresh_group_lock_mode(request_queue);
        return true;
    }

    for (auto& request : request_queue.request_queue_) {
        if (!request.granted_) {
            continue;
        }
        if (!is_compatible(lock_mode, request.lock_mode_) || !is_compatible(request.lock_mode_, lock_mode)) {
            txn->set_state(TransactionState::ABORTED);
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    request_queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
    request_queue.request_queue_.back().granted_ = true;
    txn->get_lock_set()->insert(lock_data_id);
    refresh_group_lock_mode(request_queue);
    return true;
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock_on_data(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock_on_data(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock_on_data(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock_on_data(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock_on_data(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock_on_data(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) {
        return true;
    }

    std::scoped_lock lock(latch_);
    auto table_it = lock_table_.find(lock_data_id);
    if (table_it == lock_table_.end()) {
        return false;
    }

    auto& request_queue = table_it->second;
    for (auto it = request_queue.request_queue_.begin(); it != request_queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            request_queue.request_queue_.erase(it);
            txn->get_lock_set()->erase(lock_data_id);
            if (txn->get_state() == TransactionState::GROWING) {
                txn->set_state(TransactionState::SHRINKING);
            }
            refresh_group_lock_mode(request_queue);
            if (request_queue.request_queue_.empty()) {
                lock_table_.erase(table_it);
            }
            return true;
        }
    }

    return false;
}
