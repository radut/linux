#!/usr/bin/env python3
"""boots.py SIGMA_PCT [DELTA_PCT] : boots per arm for 80% power, one-sample t on the paired ratio.

No scipy on this host, so the t critical value comes from an incomplete-beta continued fraction
(Numerical Recipes betacf) and the power from a 40k-draw simulation of the test itself.
"""
import math, random, sys

def betacf(a, b, x):
    MAXIT, EPS, FPMIN = 200, 3e-14, 1e-300
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    c, d = 1.0, 1.0 - qab * x / qap
    if abs(d) < FPMIN:
        d = FPMIN
    d = 1.0 / d
    h = d
    for m in range(1, MAXIT + 1):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < FPMIN:
            d = FPMIN
        c = 1.0 + aa / c
        if abs(c) < FPMIN:
            c = FPMIN
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < FPMIN:
            d = FPMIN
        c = 1.0 + aa / c
        if abs(c) < FPMIN:
            c = FPMIN
        d = 1.0 / d
        de = d * c
        h *= de
        if abs(de - 1.0) < EPS:
            break
    return h

def betai(a, b, x):
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    lb = (math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
          + a * math.log(x) + b * math.log1p(-x))
    if x < (a + 1.0) / (a + b + 2.0):
        return math.exp(lb) * betacf(a, b, x) / a
    return 1.0 - math.exp(lb) * betacf(b, a, 1.0 - x) / b

def t_sf2(t, nu):
    """two-sided tail area of Student t"""
    return betai(0.5 * nu, 0.5, nu / (nu + t * t))

def t_crit(nu, alpha=0.05):
    lo, hi = 0.0, 100.0
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if t_sf2(mid, nu) > alpha:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)

def power(n, sigma, delta, trials=40000, seed=1):
    if n < 2:
        return 0.0
    rnd = random.Random(seed)
    tc = t_crit(n - 1)
    hits = 0
    for _ in range(trials):
        xs = [rnd.gauss(delta, sigma) for _ in range(n)]
        m = sum(xs) / n
        var = sum((x - m) ** 2 for x in xs) / (n - 1)
        sd = math.sqrt(var)
        if sd == 0.0:
            hits += 1
            continue
        if abs(m / (sd / math.sqrt(n))) > tc:
            hits += 1
    return hits / trials

def boots(sigma, delta, want=0.80, cap=400):
    n = 2
    while n <= cap:
        if power(n, sigma, delta) >= want:
            return n
        n += 1
    return None

if __name__ == "__main__":
    sig = float(sys.argv[1])
    dl = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    n = boots(sig, dl)
    print(f"sigma={sig}% delta={dl}% -> n={n}")
