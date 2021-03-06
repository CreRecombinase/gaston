// [[Rcpp::depends(RcppEigen)]]
#include <RcppEigen.h>
#include <iostream>
#ifndef GASTONAIREMLn
#define GASTONAIREMLn
//#define ANY(_X_) (std::any_of(_X_.begin(), _X_.end(), [](bool x) {return x;})) 

using namespace Rcpp;
using namespace Eigen;

typedef Map<MatrixXd> Map_MatrixXd;


// AI REML avec n matrices de Kinship
template<typename T1, typename T2, typename A, typename T3, typename T4>
void AIREMLn(const Eigen::MatrixBase<T1> & y, const Eigen::MatrixBase<T4> & x, const std::vector<T2,A> & K, 
               int EMsteps, int EMsteps_fail, double EM_alpha, bool constraint, double min_s2, 
               const Eigen::MatrixBase<T3> & min_tau, int max_iter, double eps, bool verbose, 
               VectorXd & theta, double & logL, double & logL0, int & niter, double & gr_norm, MatrixXd & P, VectorXd & Py, 
               VectorXd & omega, VectorXd & beta, MatrixXd & XViX_i, double & varXbeta, bool start_theta) {
  int n(y.rows()), p(x.cols());
  int s(K.size());

  MatrixXd V(n,n);
  MatrixXd Vi(n,n);
  MatrixXd XViX(p,p);
  MatrixXd ViX(n,p);
  VectorXd PPy(n);

  std::vector<VectorXd> KPy, PKPy;
  for(int i = 0; i < s; i++) {
    KPy.push_back(VectorXd(n));
    PKPy.push_back(VectorXd(n));
  }

  VectorXd theta0(s+1), gr(s+1), gr_cst(s+1);
  MatrixXd AI(s+1, s+1), pi_AI(s+1,s+1);
  double log_detV, detV, old_logL, d, ld, d1, log_d1;

  // X'X
  MatrixXd xtx( MatrixXd(p,p).setZero().selfadjointView<Lower>().rankUpdate( x.transpose() ));
  MatrixXd xtxi(p,p); // et son inverse
  double det_xtx, ldet_xtx;
  MatrixXd xtx0(xtx);
  sym_inverse(xtx0, xtxi, ldet_xtx, det_xtx, 1e-5); // détruit xtx0

  // Calcul de log L0 et
  // choix paramètres initiaux
  VectorXd xty = x.transpose() * y.col(0);
  double s2_0 = (y.col(0).dot(y.col(0)) - xty.dot( xtxi*xty ))/(n-p);
  logL0 = -0.5*((n-p)*log(s2_0) + ldet_xtx + (n-p));
 
  if(!start_theta) {
    theta(0) =  s2_0/(s+1); // s2
    for(int j = 0; j < s; j++) 
      theta(j+1) = s2_0/(s+1)/K[j].diagonal().mean();
  }
  //---------------

  // booleens pour variables bloquées
  bool bloc_s2 = false;
  std::vector<bool> bloc_tau(s, false);

  bool EM = false;

  gr_norm = eps+1;
  int i;
  for(i = 0; i < max_iter; i++) {
    if(verbose) Rcout << "[Iteration " << i+1 << "] theta = " << theta.transpose() << "\n";

    V = theta(0)*MatrixXd::Identity(n,n);
    for(int j = 0; j < s; j++) V.noalias() += theta(j+1)*K[j];


   // Calcul de Vi = inverse(V)
    sym_inverse(V,Vi,log_detV,detV,1e-7);

    // Calcul de P
    ViX.noalias() = Vi * x;
    XViX.noalias() = x.transpose() * ViX;
    sym_inverse(XViX, XViX_i, log_d1, d1, 1e-5);
    P.noalias() = Vi - ViX * XViX_i * ViX.transpose();

    // Py = P * y (en tenant ompte de la symmétrie de P)
    Py.noalias()   =  P.selfadjointView<Lower>() * y;
    old_logL = logL;
    logL = -0.5*(log_detV + log_d1 + Py.dot(y.col(0)));
    if(verbose) Rcout << "[Iteration " << i+1 << "] log L = " << logL << "\n";

    // Is new value of likelihood OK ?
    if(i > 0 &&  logL < old_logL) {
      if(EM) {
        Rcout << "EM step failed to improve likelihood (this should not happen)\n";
      }
      else {
        EMsteps = EMsteps_fail;
        if(verbose) Rcout << "[Iteration " << i+1 << "] AI algorithm failed to improve likelihood\n";
        if(EMsteps > 0) {
          if(verbose) Rcout << "Trying " << EMsteps << " EM steps\n";
          // theta = theta0;
          continue;
        }
      }
    }

    // gradient

    PPy.noalias() = P.selfadjointView<Lower>() * Py;
    for(int j = 0; j < s; j++) {
      KPy[j].noalias() = K[j] * Py;
      PKPy[j].noalias() = P.selfadjointView<Lower>() * KPy[j];
    }

    gr(0) = -0.5*(P.trace() - Py.squaredNorm());
    for(int j = 0; j < s; j++) gr(j+1) = -0.5*(trace_of_product(K[j], P) - Py.dot(KPy[j]));


    // updating theta
    theta0 = theta;
    if(EMsteps > 0) {
      theta +=  theta0.cwiseProduct(theta0).cwiseProduct(gr)*2*EM_alpha/n;
      if(verbose) Rcout << "[Iteration " << i+1 << "] EM update" << "\n";
      EM = true;
      EMsteps--;
    } else {

      if(constraint) {
        // gradient contraint  
        if(bloc_s2) gr_cst(0) = 0; else gr_cst(0) = gr(0);
        for(int j = 0; j < s; j++) {
          if(bloc_tau[j]) gr_cst(j+1) = 0; else gr_cst(j+1) = gr(j+1);
        }

        // si on a convergé avec la contrainte, on regarde s'il faut débloquer des paramètres 
        // ie si le gradient ne pointe plus hors de la boîte
        if(gr_cst.norm() < eps && (bloc_s2 || any(bloc_tau))) {
          if(verbose) Rcout << "[Iteration " << i+1 << "] Checking gradient components signs before last iteration\n";
          if(bloc_s2) {
             if(gr(0) > 0) {
               bloc_s2  = false; gr_cst(0) = gr(0);
               if(verbose) Rcout << "  Releasing constraint on sigma^2\n";
             }
          }
          for(int j = 0; j < s; j++) {
            if(bloc_tau[j]) {
               if(gr(j+1) > 0) {
                 bloc_tau[j] = false; gr_cst(j+1) = gr(j+1);
                 if(verbose) Rcout << "  Releasing constraint on tau[" << j+1 << "]\n";
               }
            }
          }
        }
        gr = gr_cst;
      }

      // Average Information
      // pour variables non bloquées
      AI.setZero();
      if(!bloc_s2) AI(0,0) = 0.5*PPy.dot(Py);
      for(int j = 0; j < s; j++)
        if(!bloc_s2 && !bloc_tau[j]) AI(j+1,0)= AI(0,j+1) = 0.5*PPy.dot(KPy[j]);
      for(int j = 0; j < s; j++)
        if(!bloc_tau[j]) AI(j+1,j+1) = 0.5*PKPy[j].dot(KPy[j]);
      for(int j1 = 1; j1 < s; j1++) {
        for(int j2 = 0; j2 < j1; j2++) {
          if(!bloc_tau[j1] && !bloc_tau[j2]) AI(j1+1,j2+1) = AI(j2+1,j1+1) = 0.5*PKPy[j1].dot(KPy[j2]);
        }
      }

      // grâce aux conditions ci-dessus, les dérivées des variables bloquées ont été mises à 0
      // on prend le pseudo-inverse de AI pour faire l'itération --> ça laisse invariant les
      // variables bloquées 
      sym_inverse(AI,pi_AI,d,ld,1e-5);
      theta += pi_AI * gr;
      // --------------------------

      if(constraint) {
        double lambda = 1;
        int a_bloquer = -1; // nécessaire pour pallier aux erreurs d'arrondi après le re-calcul de theta
        if(theta(0) < min_s2) {
           double lambda0 = (min_s2 - theta0(0))/(theta(0)-theta0(0));
           if(lambda0 < lambda) { // forcément vrai ici...
              lambda = lambda0;
              a_bloquer = 0;
           }
        }
        for(int j = 0; j < s; j++) {
          if(theta(j+1) < min_tau(j)) {
            double lambda0 = (min_tau(j) - theta0(j+1))/(theta(j+1)-theta0(j+1));
            if(lambda0 < lambda) { // forcément vrai ici...
              lambda = lambda0;
              a_bloquer = j+1;
            }
          }
        }
        theta = theta0 + lambda*(theta-theta0);
        // normalement le rôle de lambda est qu'on ne devrait pas être en-dessous des minima ci-après
        // mais il faut penser aux erreurs d'arrondi... bref ceinture et bretelles
        if(theta(0) < min_s2  || a_bloquer == 0) {
          theta(0) = min_s2;  bloc_s2  = true;
          if(verbose) Rcout << "[Iteration " << i+1 << "] Constraining sigma^2\n";
        }
        for(int j = 0; j < s; j++) {
          if(theta(j+1) < min_tau(j) || a_bloquer == j+1) {
            theta(j+1) = min_tau(j); bloc_tau[j] = true;
            if(verbose) Rcout << "[Iteration " << i+1 << "] Constraining tau[" << j+1 << "]\n";
          }
        }
      }

      if(verbose) Rcout << "[Iteration " << i+1 << "] AI-REML update" << "\n";
      EM = false;
    }

    gr_norm = gr.norm();
    if(verbose) Rcout << "[Iteration " << i+1 << "] ||gradient|| = " << gr_norm << "\n";

    if(gr_norm < eps) {
      logL += gr.dot(theta-theta0);  // update linéaire de logL avant de sortir... (complètement coquet)
      break; 
    }
    R_CheckUserInterrupt();
  }
  niter = i+1;
  // Rque : l'utilisateur récupère Py qui est utile dans calcul des BLUP 
  // [l'utilisateur récupère celui qui est calculé avec le Py qui correspond à theta0 !! tant pis pour lui]
  // l'utilisateur récupère aussi logL

  // BLUP pour omega
  omega.setZero();
  for(int j = 0; j < s; j++)
    omega.noalias() += theta(j+1)*KPy[j];

  // BLUP pour beta
  // beta = (X'X)^{-1} X'(Y - omega - sigma² Py)
  beta = x.transpose() * (y - omega - theta(0)*Py);
  beta = xtxi * beta;

  // Calcul débiaisé de var (X \hat beta)
  double psi = trace_of_product(xtx, XViX_i)/(n-1);
  VectorXd gg(x.transpose()*VectorXd::Ones(n));
  psi -= gg.dot(XViX_i*gg)/n/(n-1);

  VectorXd Xbeta = x*beta;
  double SXb = Xbeta.sum();
  varXbeta = (Xbeta.squaredNorm() - SXb*SXb/n)/(n-1) - psi;

  
}
#endif

