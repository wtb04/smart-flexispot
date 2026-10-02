#include "home_model.h"

namespace ui::detail {

HomeState &home_state()
{
    static HomeState state;
    return state;
}

}  // namespace ui::detail
