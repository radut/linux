#!/bin/bash
# Wait for the whole remeasure5 run (main + extra + soaks) to finish, then regenerate the report once.
while pgrep -f "remeasure5/kit/queue.sh" >/dev/null || pgrep -f "remeasure5/kit/queue-extra.sh" >/dev/null || pgrep -f "remeasure5/kit/chain-extra.sh" >/dev/null; do sleep 120; done
cd $R5
python3 kit/combine_rm.py $S $S/dash/remeasure.json.md $S/dash/remeasure.json 2>>$R5/logs/finalize.log
python3 kit/gen_frag_rm.py $S/dash/remeasure.json $S/dash/remeasure_prose.html $S/dash/frag_remeasure.html 2>>$R5/logs/finalize.log
# rebuild reports/remeasure.md head + tables
head -n $(grep -n '^# Full tables' $S/reports/remeasure.md | head -1 | cut -d: -f1) $S/reports/remeasure.md > $S/reports/remeasure.md.new
echo >> $S/reports/remeasure.md.new
cat $S/dash/remeasure.json.md >> $S/reports/remeasure.md.new
mv $S/reports/remeasure.md.new $S/reports/remeasure.md; rm -f $S/dash/remeasure.json.md
echo "$(date -u +%FT%TZ) remeasure5: run complete; report regenerated (reports/remeasure.md, dash/remeasure.json, dash/frag_remeasure.html)" >> $S/STATUS.md
