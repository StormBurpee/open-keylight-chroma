#ifndef KEYLIGHT_CONTROLLER_WORKER_H
#define KEYLIGHT_CONTROLLER_WORKER_H
#include "controller_job.h"
/* Synchronous, sole lighting-worker job. Does not confirm a trial or open the
 * output gate; the caller performs fresh typed bootstrap and durable finish. */
okl_loader_result app_controller_worker_run(okl_nxp *driver, const app_controller_job *job,
                                          okl_loader_audit *audit,
                                          app_controller_worker_outcome *outcome);
#endif
