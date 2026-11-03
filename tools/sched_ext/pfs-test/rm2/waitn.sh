#!/bin/bash
# waitn.sh N: wait up to 590 s until loop.log has N lines; print the last 2
timeout 590 bash -c "until [ \$(wc -l < $S/rm2/res/loop.log) -ge $1 ]; do sleep 20; done"
tail -2 $S/rm2/res/loop.log; uptime
