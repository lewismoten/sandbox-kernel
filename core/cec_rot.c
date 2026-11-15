/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* Keep the CEC logs of the last three boots; see include/cec_rot.h. */
#include "cec_rot.h"
int cec_rot_step(const struct cec_rot_io *io,unsigned *step) {
 if(*step==0) {
  if(!io->has_text("CEC.TXT",io->ctx)) {*step=2;return 0;} /* nothing to keep: older boots stay where they are */
  if(io->has_text("CEC.1",io->ctx)) io->rename("CEC.1","CEC.2",io->ctx);
  *step=1;return 1;
 }
 if(*step==1) {io->rename("CEC.TXT","CEC.1",io->ctx);*step=2;}
 return 0;
}
