#include "status_model.h"

namespace ui::detail {

StatusState &status_state()
{
    static StatusState state;
    return state;
}

}  // namespace ui::detail
