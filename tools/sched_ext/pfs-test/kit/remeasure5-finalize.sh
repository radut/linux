#!/bin/bash
# Wait for the whole remeasure5 run (main + extra + soaks) to finish, then regenerate the report once.
while pgrep -f "remeasure5/kit/[q]ueue[23].sh" >/dev/null || pgrep -f "remeasure5/kit/[s]witch3.sh" >/dev/null; do sleep 120; done
cd ${PFS_WORK}/results4/remeasure5
python3 kit/combine_rm.py ${PFS_WORK} ${PFS_WORK}/dash/remeasure.json.md ${PFS_WORK}/dash/remeasure.json 2>>${PFS_WORK}/results4/remeasure5/logs/finalize.log
python3 kit/gen_frag_rm.py ${PFS_WORK}/dash/remeasure.json ${PFS_WORK}/dash/remeasure_prose.html ${PFS_WORK}/dash/frag_remeasure.html 2>>${PFS_WORK}/results4/remeasure5/logs/finalize.log
# rebuild reports/remeasure.md head + tables
head -n $(grep -n '^# Full tables' ${PFS_WORK}/reports/remeasure.md | head -1 | cut -d: -f1) ${PFS_WORK}/reports/remeasure.md > ${PFS_WORK}/reports/remeasure.md.new
echo >> ${PFS_WORK}/reports/remeasure.md.new
cat ${PFS_WORK}/dash/remeasure.json.md >> ${PFS_WORK}/reports/remeasure.md.new
mv ${PFS_WORK}/reports/remeasure.md.new ${PFS_WORK}/reports/remeasure.md; rm -f ${PFS_WORK}/dash/remeasure.json.md
echo "$(date -u +%FT%TZ) remeasure5: run complete; report regenerated (reports/remeasure.md, dash/remeasure.json, dash/frag_remeasure.html)" >> ${PFS_WORK}/STATUS.md