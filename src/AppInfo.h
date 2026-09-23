// Copyright (c) 2026 re-v0
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QString>

#ifndef BLAIRQT_VERSION
#define BLAIRQT_VERSION "0.0.0-dev"
#endif

// One User-Agent for every request. Sites like e621 require one that names
// the client, and it has to track the real version, not a frozen literal.
inline QString blairqtUserAgent()
{
    return QStringLiteral("blairqt/" BLAIRQT_VERSION " (Qt6 booru client)");
}
