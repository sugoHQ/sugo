// SPDX-License-Identifier: GPL-3.0-or-later
#include "QuitPolicy.h"

namespace QuitPolicy
{
bool needsConfirmation(Origin origin, bool phaseActive, bool unsavedData)
{
    return origin == Origin::External && (phaseActive || unsavedData);
}
} // namespace QuitPolicy
