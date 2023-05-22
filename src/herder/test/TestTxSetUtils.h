// Copyright 2022 Hcnet Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "herder/TxSetUtils.h"
#include "xdr/Hcnet-ledger.h"

namespace hcnet
{
namespace testtxset
{
TxSetFrameConstPtr makeNonValidatedGeneralizedTxSet(
    std::vector<std::pair<std::optional<int64_t>,
                          std::vector<TransactionFrameBasePtr>>> const&
        txsPerBaseFee,
    Application& app, Hash const& previousLedgerHash);

TxSetFrameConstPtr makeNonValidatedTxSetBasedOnLedgerVersion(
    uint32_t ledgerVersion, std::vector<TransactionFrameBasePtr> const& txs,
    Application& app, Hash const& previousLedgerHash);
} // namespace testtxset
} // namespace hcnet