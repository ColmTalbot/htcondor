executable   = /bin/echo
arguments    = OK
universe     = scheduler
output       = job_dagman_splice-N-subdir2/a/$(job).out
error        = job_dagman_splice-N-subdir2/a/$(job).err
log          = job_dagman_splice-N-subdir2/a/a_submit.log
Notification = NEVER
should_transfer_files = NO
queue
