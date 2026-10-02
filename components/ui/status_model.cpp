#include "status_model.h"

#include "topics.h"

namespace ui::detail {

StatusState &status_state()
{
    static StatusState state;
    return state;
}

void status_splash_gone()
{
    status_state().splash_gone = true;
    publish(Topic::Status);
}

}  // namespace ui::detail
