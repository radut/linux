#!/bin/bash
# vm.sh NAME KARGS VARIANT ROUND PHASES : boot the variant's image in virtme-ng (8 vCPU, 4G) and run guest.sh
# kpfs -> kpfs/build-r5 (e6d161de0926), kpfsold -> kpfs/build-p5 (phase 5), others -> kpfs/build-perf (as before)
S="${PFS_WORK:?set PFS_WORK to a writable work directory}"
R=$S/remeasure
name=$1 kargs=$2; shift 2
QO= GE=
case $1 in kpfs) B=$S/kpfs/build-r5 ;; kpfsold) B=$S/kpfs/build-p5 ;; kpfsnoext) B=$S/kpfs/build-xnoext ;;
kpfsrt) B=$S/kpfs/build-xrt ;; kpfskcsan) B=$S/kpfs/build-xkcsan ;;
kpfscore) B=$S/kpfs/build-r5; GE="CORE=1" ;; kpfssmt) B=$S/kpfs/build-r5 ;; *) B=$S/kpfs/build-perf ;; esac
# SMT topology (4 cores x 2 threads) for the core-scheduling runs and their kpfs reference (SMT=1)
[ "$1" = kpfscore ] || [ "$1" = kpfssmt ] || [ "${SMT:-0}" = 1 ] && QO="--qemu-opts=-smp 8,sockets=1,cores=4,threads=2"
tmo=${TMO:-2700}
O=${RESD:-$R/resx}; mkdir -p $O
rm -f $O/$name.txt $O/$name.console
[ "$kargs" = "-" ] && kargs=""
cat > $O/$name.guest.sh <<EOG
( env PFS_WORK=$S $GE bash $R/guest.sh $* ) 2>&1 | tee $O/$name.txt
dmesg > $O/$name.dmesg
EOG
cat > $O/$name.run.sh <<EOG
cd $B
exec vng --run $B --user root --cpus 8 ${QO:+'$QO'} --memory 4G --rwdir=$S --append "$kargs" --exec "bash $O/$name.guest.sh"
EOG
timeout -k 10 $tmo script -q -f -e -c "sg kvm -c 'bash $O/$name.run.sh'" $O/$name.console < /dev/null > /dev/null 2>&1
rc=$?
echo "vm exit=$rc build=${B##*/}" >> $O/$name.txt
exit $rc