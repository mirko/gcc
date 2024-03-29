/* PR middle-end/97172 - ICE: tree code ‘ssa_name’ is not supported in LTO
   streams
   { dg-do compile }
   { dg-options "-Wall -flto" }
   { dg-require-effective-target lto } */

int n;

void fn (int a[n]);
void fnp1 (int a[n + 1]);

void fx_n (int a[][n]);
void fx_np1 (int a[][n + 1]);

void f2_n (int a[2][n]);
void f2_np1 (int a[2][n + 1]);

void fn_3 (int a[n][3]);
void fnp1_3 (int a[n + 1][3]);

void fn_n (int a[n][n]);
void fn_np1 (int a[n][n + 1]);
void fnp1_np1 (int a[n + 1][n + 1]);

void fn_n_n (int a[n][n][n]);
void fn_n_np1 (int a[n][n][n + 1]);
void fn_np1_np1 (int a[n][n + 1][n + 1]);
void fnp1_np1_np1 (int a[n + 1][n + 1][n + 1]);


void gn (int a[n]) { fn (a); }
void gnp1 (int a[n + 1]) { fnp1 (a); }

void gx_n (int a[][n]) { fx_n (a); }
void gx_np1 (int a[][n + 1]) { fx_np1 (a); }

void g2_n (int a[2][n]) { f2_n (a); }
void g2_np1 (int a[2][n + 1]) { f2_np1 (a); }

void gn_3 (int a[n][3]) { fn_3 (a); }
void gnp1_3 (int a[n + 1][3]) { fnp1_3 (a); }

void gn_n (int a[n][n]) { fn_n (a); }
void gn_np1 (int a[n][n + 1]) { fn_np1 (a); }
void gnp1_np1 (int a[n + 1][n + 1]) { fnp1_np1 (a); }

void gn_n_n (int a[n][n][n]) { fn_n_n (a); }
void gn_n_np1 (int a[n][n][n + 1]) { fn_n_np1 (a); }
void gn_np1_np1 (int a[n][n + 1][n + 1]) { fn_np1_np1 (a); }
void gnp1_np1_np1 (int a[n + 1][n + 1][n + 1]) { fnp1_np1_np1 (a); }
