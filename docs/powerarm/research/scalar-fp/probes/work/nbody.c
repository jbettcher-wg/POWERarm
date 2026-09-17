/* nbody: the classic 5-body double-precision integrator (fmul/fadd/fsub,
 * fdiv, fsqrt, fmadd/fmsub from contraction). Scalar FP arithmetic bound. */
#include "fpbench.h"
#define N 5
typedef struct { double x, y, z, vx, vy, vz, m; } body;
static void advance(body* b, double dt) {
  for (int i = 0; i < N; i++) for (int j = i + 1; j < N; j++) {
    double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
    double d2 = dx * dx + dy * dy + dz * dz;
    double mag = dt / (d2 * sqrt(d2));
    b[i].vx -= dx * b[j].m * mag; b[i].vy -= dy * b[j].m * mag; b[i].vz -= dz * b[j].m * mag;
    b[j].vx += dx * b[i].m * mag; b[j].vy += dy * b[i].m * mag; b[j].vz += dz * b[i].m * mag;
  }
  for (int i = 0; i < N; i++) { b[i].x += dt * b[i].vx; b[i].y += dt * b[i].vy; b[i].z += dt * b[i].vz; }
}
static double energy(body* b) {
  double e = 0;
  for (int i = 0; i < N; i++) {
    e += 0.5 * b[i].m * (b[i].vx * b[i].vx + b[i].vy * b[i].vy + b[i].vz * b[i].vz);
    for (int j = i + 1; j < N; j++) {
      double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
      e -= b[i].m * b[j].m / sqrt(dx * dx + dy * dy + dz * dz);
    }
  }
  return e;
}
static uint64_t run(long scale) {
  const double PI = 3.141592653589793, SM = 4 * PI * PI, DPY = 365.24;
  body b[N] = {
    { 0, 0, 0, 0, 0, 0, SM },
    { 4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01, 1.66007664274403694e-03 * DPY, 7.69901118419740425e-03 * DPY, -6.90460016972063023e-05 * DPY, 9.54791938424326609e-04 * SM },
    { 8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01, -2.76742510726862411e-03 * DPY, 4.99852801234917238e-03 * DPY, 2.30417297573763929e-05 * DPY, 2.85885980666130812e-04 * SM },
    { 1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01, 2.96460137564761618e-03 * DPY, 2.37847173959480950e-03 * DPY, -2.96589568540237556e-05 * DPY, 4.36624404335156298e-05 * SM },
    { 1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01, 2.68067772490389322e-03 * DPY, 1.62824170038242295e-03 * DPY, -9.51592254519715870e-05 * DPY, 5.15138902046611451e-05 * SM } };
  double px = 0, py = 0, pz = 0;
  for (int i = 0; i < N; i++) { px += b[i].vx * b[i].m; py += b[i].vy * b[i].m; pz += b[i].vz * b[i].m; }
  b[0].vx = -px / SM; b[0].vy = -py / SM; b[0].vz = -pz / SM;
  uint64_t h = hash_double(0, energy(b));
  for (long i = 0; i < scale; i++) advance(b, 0.01);
  h = hash_double(h, energy(b));
  for (int i = 0; i < N; i++) h = hash_double(hash_double(h, b[i].x), b[i].vz);
  return h;
}
FPBENCH_MAIN(run, 2000000)
