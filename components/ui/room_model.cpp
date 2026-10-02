#include "room_model.h"

namespace ui::detail {

DeskState &desk_state()
{
    static DeskState state;
    return state;
}

LightsState &lights_state()
{
    static LightsState state;
    return state;
}

}  // namespace ui::detail
