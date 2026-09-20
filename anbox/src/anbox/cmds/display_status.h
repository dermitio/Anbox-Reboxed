#ifndef ANBOX_CMDS_DISPLAY_STATUS_H_
#define ANBOX_CMDS_DISPLAY_STATUS_H_

#include "anbox/cli.h"

namespace anbox::cmds {
class DisplayStatus : public cli::CommandWithFlagsAndAction {
 public:
  DisplayStatus();
};
}
#endif
