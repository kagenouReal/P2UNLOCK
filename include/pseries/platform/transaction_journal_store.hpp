#pragma once
#include "pseries/recovery.hpp"
#include <string>
namespace pseries::platform {
[[nodiscard]] Result<void> save_transaction_journal(
const std::wstring& path,
const P104TransactionJournal& journal);
[[nodiscard]] Result<P104TransactionJournal> load_transaction_journal(
const std::wstring& path);
[[nodiscard]] Result<void> clear_transaction_journal(const std::wstring& path);
}
