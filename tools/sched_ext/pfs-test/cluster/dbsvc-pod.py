#!/usr/bin/env python3
"""Render one bare Pod (JSON) for the scx-fix tests in namespace sched-test.

pod.py NAME NODE(a|b) IMAGE [--cpu L] [--mem M] [--req-cpu R] [--env K=V]... [--shm SIZE]
       [--data PATH] [--hostpath-scx] [--cm NAME:PATH] [--role ROLE] -- CMD ARGS...

--cpu L sets requests = limits = L (Guaranteed); without it the pod is BestEffort
(unless --req-cpu gives a request without a limit).
"""
LABEL_DOMAIN = os.environ.get("LABEL_DOMAIN", "pfs-test.example")

import os
import argparse
import json
import sys

LABELS = {
    "%s/" % LABEL_DOMAIN + "out-of-band": "temporary",
    "%s/" % LABEL_DOMAIN + "owner": "sched-ext-test",
    "%s/" % LABEL_DOMAIN + "expires": "2026-11-06",
    "app.kubernetes.io/name": "scx-fix",
}
REASON = "sched_ext scx_pfs fix trial: database and service latency under CPU limits"
ROLE = {"a": "reference", "b": "scx"}


def main():
    argv = sys.argv[1:]
    cmd = []
    if "--" in argv:
        i = argv.index("--")
        argv, cmd = argv[:i], argv[i + 1:]
    p = argparse.ArgumentParser()
    p.add_argument("name")
    p.add_argument("node", choices=["a", "b"])
    p.add_argument("image")
    p.add_argument("--cpu")
    p.add_argument("--req-cpu")
    p.add_argument("--mem")
    p.add_argument("--env", action="append", default=[])
    p.add_argument("--shm")
    p.add_argument("--data")
    p.add_argument("--hostpath-scx", action="store_true")
    p.add_argument("--cm", action="append", default=[])
    p.add_argument("--role", default="server")
    p.add_argument("--deadline", type=int, default=3 * 3600)
    a = p.parse_args(argv)

    labels = dict(LABELS, **{"scx-fix/role": a.role, "scx-fix/pod": a.name})
    c = {"name": "main", "image": a.image, "imagePullPolicy": "IfNotPresent",
         "env": [{"name": k, "value": v} for k, v in (e.split("=", 1) for e in a.env)],
         "volumeMounts": []}
    if cmd:
        c["command"] = cmd
    res = {}
    if a.cpu:
        res = {"requests": {"cpu": a.cpu}, "limits": {"cpu": a.cpu}}
        if a.mem:
            res["requests"]["memory"] = res["limits"]["memory"] = a.mem
    elif a.req_cpu:
        res = {"requests": {"cpu": a.req_cpu}}
    if res:
        c["resources"] = res
    vols = []
    if a.shm:
        vols.append({"name": "shm", "emptyDir": {"medium": "Memory", "sizeLimit": a.shm}})
        c["volumeMounts"].append({"name": "shm", "mountPath": "/dev/shm"})
    if a.data:
        vols.append({"name": "data", "emptyDir": {}})
        c["volumeMounts"].append({"name": "data", "mountPath": a.data})
    if a.hostpath_scx:
        vols.append({"name": "scx", "hostPath": {"path": "/opt/scx", "type": "Directory"}})
        c["volumeMounts"].append({"name": "scx", "mountPath": "/opt/scx", "readOnly": True})
    for spec in a.cm:
        cm, path = spec.split(":", 1)
        vols.append({"name": cm, "configMap": {"name": cm}})
        c["volumeMounts"].append({"name": cm, "mountPath": path, "readOnly": True})

    pod = {
        "apiVersion": "v1", "kind": "Pod",
        "metadata": {"name": a.name, "namespace": "sched-test", "labels": labels,
                     "annotations": {"%s/" % LABEL_DOMAIN + "reason": REASON}},
        "spec": {
            "nodeSelector": {"sched-test": "true", "sched-test/role": ROLE[a.node]},
            "tolerations": [{"key": "sched-test", "operator": "Equal", "value": "true",
                             "effect": "NoSchedule"}],
            "restartPolicy": "Never",
            "activeDeadlineSeconds": a.deadline,
            "terminationGracePeriodSeconds": 5,
            "enableServiceLinks": False,
            "automountServiceAccountToken": False,
            "containers": [c],
            "volumes": vols,
        },
    }
    json.dump(pod, sys.stdout, indent=1)
    print()


if __name__ == "__main__":
    main()
