// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Infinite scroll is driven by scrollbar movement, so a round that adds
// nothing (a fully blacklisted page) or that does not fill the viewport
// produces no further scroll events and loading would stop silently.
// This decides when to pull one more page by hand instead.
//
// chainCount counts consecutive self-triggered loads; the limit stops a
// runaway when a source keeps returning posts the blacklist removes.
inline bool shouldChainAutoLoad(bool infiniteScroll, int rawResults, int appendedResults,
                                int scrollMaximum, int chainCount, int chainLimit)
{
    if (!infiniteScroll)
        return false;
    if (rawResults <= 0)
        return false; // the source is exhausted; nothing more to ask for
    if (chainCount >= chainLimit)
        return false;
    return appendedResults == 0 || scrollMaximum <= 0;
}
