#include "ls_actions.h"
#include "ls_control.h"
unsigned ls_actions(bool connected, bool compatible, bool known, bool recipe, bool busy,
                    bool updating, uint8_t state, uint8_t flags) {
    if (busy || updating)
        return 0;
    bool ready = connected && compatible && known;
    bool idle = !connected || !compatible || (known && state == LsStateIdle);
    unsigned allowed = LsCanExit;
    if (idle)
        allowed |= LsCanLoad | LsCanSettings | LsCanWifi;
    if (!ready)
        return allowed;
    if (state == LsStateIdle && recipe && (flags & LsFlagRecipe))
        allowed |= LsCanStart;
    if (recipe && (flags & LsFlagRecipe) && (flags & LsFlagPeer) &&
        (state == LsStateConnected || state == LsStateComplete))
        allowed |= LsCanOffer;
    if (state == LsStateOffered)
        allowed |= LsCanCancel;
    if (state != LsStateIdle && state != LsStateStopping && !(flags & LsFlagStopRequested))
        allowed |= LsCanStop;
    return allowed;
}
