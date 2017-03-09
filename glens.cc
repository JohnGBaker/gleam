//Gravitational lens equation for microlensing
//Written by John G Baker NASA-GSFC (2014-2016)

#include "glens.hh"
#include <gsl/gsl_poly.h>
#include <gsl/gsl_odeiv2.h>
#include <gsl/gsl_errno.h>
#include <cmath>
#include <algorithm>
#include <complex>
#ifdef USE_KIND_16
#include <quadmath.h>
#else
#define __float128 long double
#endif

const bool fix_nr4_roots=true;
const bool inv_test_mode=false;
//const bool inv_test_mode=true;
extern bool debugint;
bool verbose=false;
bool test_result=false;
bool save_thetas_poly=true;
//bool save_thetas_poly=false;
//bool save_thetas_wide=true;  Strangely, this seems to provide no advantage.  Maybe a bug, but didn't see it. 
bool save_thetas_wide=false; 
//
// Interface to external Skowron&Gould fortran polynomial solver routine
//

#ifndef USE_KIND_16

extern "C" void cmplx_roots_5_(complex<double> roots[5], int &first_3_roots_order_changed, complex<double> poly[6], const int &polish_only);
void cmplx_roots_5(complex<double> roots[5], bool &first_3_roots_order_changed, complex<double> poly[6], const bool &polish_only){
  int first3=first_3_roots_order_changed;
  cmplx_roots_5_(roots, first3, poly, polish_only);
  first_3_roots_order_changed=first3;
};
extern "C" void cmplx_roots_gen_(complex<double> roots[], complex<double> poly[], const int &degree, const int &polish_roots_after, const int &use_roots_as_starting_points);
void cmplx_roots_gen(complex<double> roots[], complex<double> poly[], const int &degree, bool const &polish_roots_after, const bool &use_roots_as_starting_points){cmplx_roots_gen_(roots,poly,degree,polish_roots_after,use_roots_as_starting_points);};
typedef  long double ldouble;
const double LEADTOL=1e-5;
//const double LEADTOL=1e-4;
//const double epsTOL=1e-15;
const double epsTOL=1e-14;
//const double epsTOL=1e-11;

#else

extern "C" void cmplx_roots_5_(complex<__float128> roots[5], int &first_3_roots_order_changed, complex<__float128> poly[6], const int &polish_only);
void cmplx_roots_5(complex<double> roots[5], bool &first_3_roots_order_changed, complex<double> poly[6], const bool &polish_only){
  int first3=first_3_roots_order_changed;
  complex<__float128>longroots[5],longpoly[6];
  for(int i=0;i<6;i++)longpoly[i]=poly[i];
  if(!polish_only)for(int i=0;i<5;i++)longroots[i]=roots[i];
  //cout<<sizeof(longpoly)<<endl;
  cmplx_roots_5_(longroots, first3, longpoly, polish_only);
  for(int i=0;i<5;i++)roots[i]=longroots[i];  
  first_3_roots_order_changed=first3;
};
extern "C" void cmplx_roots_gen_(complex<__float128> roots[], complex<__float128> poly[], const int &degree, const int &polish_roots_after, const int &use_roots_as_starting_points);
void cmplx_roots_gen(complex<double> roots[], complex<double> poly[], const int &degree, const bool &polish_roots_after, const bool &use_roots_as_starting_points){
  complex<__float128>longroots[5],longpoly[6];
  for(int i=0;i<degree+1;i++)longpoly[i]=poly[i];
  /*char c[128];
  for(int i=0;i<degree+1;i++){
    quadmath_snprintf (c, sizeof c, "%-#*.35Qe", 46, longpoly[i]);  
    cout<<"poly["<<i<<"]="<<c<<endl;  
    }*/
  if(use_roots_as_starting_points)for(int i=0;i<degree;i++)longroots[i]=roots[i];
  //cout<<sizeof(longpoly)<<endl;
  int deg=degree;
  cmplx_roots_gen_(longroots,longpoly,deg,polish_roots_after,use_roots_as_starting_points);
  for(int i=0;i<degree;i++)roots[i]=longroots[i];  
};
typedef __float128 ldouble;
const double LEADTOL=3e-7;
//const double LEADTOL=1e-5;
const double epsTOL=1e-18;
//const double epsTOL=1e-14;
#endif



//
// ******************************************************************
// GLens routines ***************************************************
// ******************************************************************
//

//Use GSL routine to integrate 
void GLens::compute_trajectory (const Trajectory &traj, vector<double> &time_series, vector<vector<Point> > &thetas_series, vector<int> &index_series,vector<double>&mag_series,bool integrate)
{
  // Given a trajectory through the observer plane, and a list of observation times, integrate the Jacobian to yield the corresponding trajectory in the lens plane.
  //
  //Arguments:
  //
  //Trajectory traj       -provides information about the trajectory of early thorough the observer plane.
  //traj->times  -provides a list of observation times to be included in the sample set 
  //vector<double> theta  -yields the resulting thetas for all sample times
  //vector<int> grid_idxs -has lenght of "times" and yield index values for the location of the corresponding results within the larger "theta" vector.
  //bool integrate (false for direct polynomial evaluation rather than integration. 
  //  if use_integrate is set then the value it overrides integrate 
  //
  //control parameters:
  double caustic_mag_poly_level = GL_int_mag_limit; //use direct polynomial eval near caustics.
  const double intTOL = GL_int_tol;  //control integration error tolerance
  const double test_result_tol = 1e-4;  //control integration error tolerance
  if(have_integrate)integrate=use_integrate;
  double prec=cout.precision();cout.precision(20);
  const double rWide_int_fac=100.0;

  
  //cout<<"glens::compTraj: int="<<integrate<<"\nthisLens="<<print_info()<<"\n traj="<<traj.print_info()<<endl;
  //cout<<"this="<<this<<endl;
  cout.precision(prec);

  ///clear the outputs
  time_series.clear();
  thetas_series.clear();
  index_series.clear();
  mag_series.clear();

  trajectory=&traj;//a convenience for passing to the integrator
  int NintSize=2*NimageMax;

  //Set up for GSL integration routines
  const gsl_odeiv2_step_type * stepType = gsl_odeiv2_step_rkf45;
  gsl_odeiv2_step * step=nullptr;
  gsl_odeiv2_control * control=nullptr;
  gsl_odeiv2_evolve * evol=nullptr;
  gsl_odeiv2_system sys = {GLens::GSL_integration_func_vec, NULL, (size_t)NintSize, this};
  double h=1e-5;
  if(integrate){
    step = gsl_odeiv2_step_alloc (stepType, NintSize);
    control = gsl_odeiv2_control_y_new (intTOL, 0.0);
    //control = gsl_odeiv2_control_standard_new (intTOL, 0.0,1.0,10.0);//Also apply limits on the derivative
    evol = gsl_odeiv2_evolve_alloc (NintSize);
    gsl_odeiv2_control_init(control,intTOL,0.0,1,0);//Do I need this?
    gsl_odeiv2_evolve_reset(evol);//or this
  }

  //Main loop over observation times specified in the Trajectory object
  //initialization
  Point beta;
  vector<Point> thetas;
  bool evolving=false;
  double mg;
  have_saved_soln=false;

  int Ngrid=traj.Nsamples();
  double t_old;
  for(int i=0; i<Ngrid;i++){
    double tgrid=traj.get_obs_time(i);
    //entering main loop

    //We either compute the next step by evolution or by polynomial evaluation.
    //We do evolution is the integration flag is set *and* the last evaluated point had a
    //magnification under caustic_mag_poly_level.
    //If we evolving then the step-size may be smaller as driven by the ODE integrator's step-size 
    //control, we expect this to be small enough to avoide stumbling across the caustic, unaware.
    //After each step we check whether the mg level condition is satisfied, to consider whether to evolve.
    //We may also wish to implement a floor in the step-size near caustics for display purposes.

    if(evolving){
      double t=t_old;
      //The next loop steps (probably more finely) toward the next grid time point.      
      while(t<tgrid){
	//integrate each image
	if(debugint)cout<<"\n t="<<t<<" mg="<<mg<<endl;
	Ntheta=thetas.size();
	double theta[NintSize];
	for(int image=0;image<thetas.size();image++){
	  theta[2*image]=thetas[image].x;
	  theta[2*image+1]=thetas[image].y;
	}
	if(rWide_int_fac>0){
	  //Point b=traj.get_obs_pos(t);//TRAJ:Allow non-trivial transformation from observer-plane coords frame to lens frame coords
	  Point b=get_obs_pos(traj,t);
	  if(testWide(b,rWide_int_fac)){
	    //cout<<"evol: testWide==true"<<endl;
	    evolving=false;//switch to point-wise (WideBinary assuming rWide_int/rWide>=1)
	    break;
	  }
	}
	for(int k=2*thetas.size();k<NintSize;k++)theta[k]=0;
	int status = gsl_odeiv2_evolve_apply (evol, control, step, &sys, &t, tgrid, &h, theta);
	//Need some step-size control checking for near caustics?
	if (status != GSL_SUCCESS) { 
	  evolving=false;//switch to polynomial
	  break;
	}
	//record results
	for(int image=0;image<thetas.size();image++){
	  thetas[image]=Point(theta[2*image],theta[2*image+1]);	    
	}
	mg=mag(thetas);
	if(mg>=caustic_mag_poly_level)evolving=false;//switch to polynomial and don't record result
	else {
	  if(debugint)cout<<"mg="<<mg<<endl;
	  time_series.push_back(t);
	  thetas_series.push_back(thetas);
	  mag_series.push_back(mg);
	  if(!isfinite(mg)){
	    cout<<"integrate: mg is infinite!\n";
	    for(int image=0;image<thetas.size();image++){
	      cout<<theta[2*image]<<","<<theta[2*image+1]<<endl;
	    }	    
	  }
	}
      }
      if(!evolving){
	i--;//go back and try this step again with solving polynomial
	continue;
      }
    } else { //not evolving, solve polynomial
      beta=get_obs_pos(traj,tgrid);
      //cout<<"Not evolving: beta=("<<beta.x<<","<<beta.y<<")"<<endl;
      thetas.clear();
      thetas=invmap(beta);
      //record results;
      mg=mag(thetas);
      time_series.push_back(tgrid);
      thetas_series.push_back(thetas);
      mag_series.push_back(mg);
      //cout<<"mg="<<mg<<", caustic_mag_poly_level="<<caustic_mag_poly_level<<endl;
      if(integrate&&mg<caustic_mag_poly_level){
	double r2=beta.x*beta.x+beta.y*beta.y;
	evolving=true;
	if(testWide(beta,rWide_int_fac))evolving=false;//Don't switch if in "wide" domain.
	//if(!evolving)cout<<"not evol: testWide==true"<<endl;
	if(!(thetas.size()==3||thetas.size()==5))evolving=false;//Don't switch to integrate if the number of images doesn't make sense
	if(evolving)gsl_odeiv2_evolve_reset(evol);
      };
      if(!isfinite(mg)){
	GLensBinary* gb;
	bool squak=true;
	if(gb=dynamic_cast< GLensBinary* > (this)){
	  if(gb->get_q()<1e-14)squak=false;//we are going to fail with NAN, but not give a bunch of output, deal with it...
	  else cout<<"q,L="<<gb->get_q()<<","<<gb->get_L()<<endl;
	}
	if(squak){
	  cout<<"!integrate: mg is infinite! at beta="<<beta.x<<","<<beta.y<<endl;
	  for(int image=0;image<thetas.size();image++){
	    cout<<thetas[image].x<<","<<thetas[image].y<<" -> "<<mag(thetas[image])<<endl;
	  }
	  Trajectory::verbose=true;
	  beta=get_obs_pos(traj,tgrid);
	  alert();	 
	  Trajectory::verbose=false;
	}
      }
      if(debugint){
	cout<<"polynomial calc at t="<<tgrid<<":"<<endl;
	for(int image=0;image<thetas.size();image++)
	  cout<<"   theta["<<image<<"] = ("<<thetas[image].x<<","<<thetas[image].y<<")"<<endl;
      }
    }//end of polynomial step
    if(time_series.size()<1)cout<<"Time series empty i="<<i<<endl;
    t_old=tgrid;
    index_series.push_back(time_series.size()-1);
  }//end of main observation times loop

  if(integrate){
    gsl_odeiv2_evolve_free (evol);
    gsl_odeiv2_control_free (control);
    gsl_odeiv2_step_free (step);
  }

  if(test_result){
  //initialization
    for(int i=0; i<Ngrid;i++){
      double ttest=traj.get_obs_time(i);
      int ires=index_series[i];
      double tres=time_series[ires];
      Point beta=get_obs_pos(traj,ttest);
      vector<Point> thetas=invmap(beta);
      int nimages=thetas.size();
      double mgtest=mag(thetas);
      double mgres=mag_series[ires];
      double tminus,tplus,mgminus,mgplus;
      if(ires>0&&ires<time_series.size()-1){
	tminus=time_series[ires-1];
	tplus=time_series[ires+1];
	beta=get_obs_pos(traj,tminus);
	thetas=invmap(beta);
	mgminus=mag(thetas);
	beta=get_obs_pos(traj,tplus);
	thetas=invmap(beta);
	mgplus=mag(thetas);
      }

      if(abs(mgtest-mgres)/mgtest>test_result_tol)
#pragma omp critical
	{
	cout<<"\ncompute_trajectory: test failed. test/res:\nindex="<<i<<","<<ires<<"\ntime="<<ttest<<","<<tres<<"\nmag="<<mgtest<<","<<mgres<<" -> "<<abs(mgtest-mgres)<<"["<<nimages<<" images]"<<endl;
	cout<<"beta=("<<beta.x<<","<<beta.y<<")"<<endl;
	if(ires>0&&ires<time_series.size()-1){
	  cout<<"nearby times  :"<<tminus<<" < t < "<<tplus<<endl;
	  cout<<"   with mags  :"<<mgminus<<" < mg < "<<mgplus<<endl;
	}
	//if(i>0&&ires<index_series.size()-1)cout<<"nearby idx times:"<<time_series[index_series[i-1]]<<" < t < "<<time_series[index_series[i+1]]<<endl;
      }
    }
  }

}

//This version seems no longer used 04.02.2016..
int GLens::GSL_integration_func (double t, const double theta[], double thetadot[], void *instance){
  //We make the following cast static, thinking that that should realize faster integration
  //However, since we call functions which are virtual, this may/will give the wrong result if used
  //with a derived class that has overloaded those functions {get_obs_pos, get_obs_vel, invjac, map}
  GLens *thisobj = static_cast<GLens *>(instance);
  const Trajectory *traj=thisobj->trajectory;
  Point p(theta[0],theta[1]);
  double j00i,j10i,j01i,j11i,invJ = thisobj->invjac(p,j00i,j01i,j10i,j11i);
  Point beta0=thisobj->get_obs_pos(*traj,t);
  Point beta=thisobj->map(p);
  double dx=beta.x-beta0.x,dy=beta.y-beta0.y;
  Point betadot=thisobj->get_obs_vel(*traj,t);
  double adjbetadot[2];
  //double rscale=thisobj->estimate_scale(Point(theta[0],theta[1]));
  //if our image is near one of the lenses, then we need to tread carefully
  double rk=thisobj->kappa;//*rscale*0;
  adjbetadot[0] = betadot.x-rk*dx;
  adjbetadot[1] = betadot.y-rk*dy;
  thetadot[0] = j00i*adjbetadot[0]+j01i*adjbetadot[1];
  thetadot[1] = j10i*adjbetadot[0]+j11i*adjbetadot[1];

  if(!isfinite(invJ))cout<<"GLens::GSL_integration_func: invJ=inf, |beta|="<<sqrt(beta0.x*beta0.x+beta0.y*beta0.y)<<endl;

  return isfinite(invJ)?GSL_SUCCESS:3210123;
}


int GLens::GSL_integration_func_vec (double t, const double theta[], double thetadot[], void *instance){
  //We make the following cast static, thinking that that should realize faster integration
  //However, since we call functions which are virtual, this may/will give the wrong result if used
  //with a derived class that has overloaded those functions {get_obs_pos, get_obs_vel, invjac, map}
  GLens *thisobj = static_cast<GLens *>(instance);
  const Trajectory *traj=thisobj->trajectory;
  Point beta0=thisobj->get_obs_pos(*traj,t);
  Point betadot=thisobj->get_obs_vel(*traj,t);
  bool fail=false;
  for(int k=0;k<2*thisobj->NimageMax;k++)thetadot[k]=0;
  //cout<<"beta0=("<<beta0.x<<","<<beta0.y<<")"<<endl;
  for(int image =0; image<thisobj->Ntheta;image++){
    Point p(theta[image*2+0],theta[image*2+1]);
    double j00i,j10i,j01i,j11i,invJ = thisobj->invjac(p,j00i,j01i,j10i,j11i);
    Point beta=thisobj->map(p);
    double dx=beta.x-beta0.x,dy=beta.y-beta0.y;
    double adjbetadot[2];
    //double rscale=thisobj->estimate_scale(Point(theta[0],theta[1]));
    //if our image is near one of the lenses, then we need to tread carefully
    double rk=thisobj->kappa;//*rscale;
    adjbetadot[0] = betadot.x;
    adjbetadot[1] = betadot.y;
    if(isfinite(dx))adjbetadot[0] += -rk*dx;
    if(isfinite(dy))adjbetadot[1] += -rk*dy;
    if(isfinite(invJ)){
      thetadot[2*image]   = (j00i*adjbetadot[0]+j01i*adjbetadot[1]);
      thetadot[2*image+1] = (j10i*adjbetadot[0]+j11i*adjbetadot[1]);
    }
    if(!isfinite(invJ)){    
      //cout<<"GLens::GSL_integration_func_vec: FAIL"<<endl;
      //fail=true;
      //here we panic and rather than return NAN, we evolve as if the lensing effect is trivial...
      cout<<"GLens::GSL_integration_func_vec: invJ=inf, |beta|="<<sqrt(beta0.x*beta0.x+beta0.y*beta0.y)<<endl;
      thetadot[2*image]   = betadot.x;
      thetadot[2*image+1] = betadot.y;
    }
  }
  for(int k=2*thisobj->Ntheta;k<2*thisobj->NimageMax;k++)thetadot[k]=0;

  return !fail?GSL_SUCCESS:3210123;
}

void GLens::addOptions(Options &opt,const string &prefix){
  Optioned::addOptions(opt,prefix);
  //addTypeOptions(opt);
  opt.add(Option("GL_poly","Don't use integration method for lens magnification, use only the polynomial method."));
  opt.add(Option("poly","Same as GL_poly for backward compatibility.  (Deprecated)"));
  opt.add(Option("GL_int_tol","Tolerance for GLens inversion integration. (1e-10)","1e-10"));
  opt.add(Option("GL_int_mag_limit","Magnitude where GLens inversion integration reverts to poly. (1.5)","1.5"));
  opt.add(Option("GL_int_kappa","Strength of driving term for GLens inversion. (0.1)","0.1"));
};

void GLens::setup(){
  set_integrate(!optSet("GL_poly")&&!optSet("poly"));
  *optValue("GL_int_tol")>>GL_int_tol;
  *optValue("GL_int_mag_limit")>>GL_int_mag_limit;
  *optValue("GL_int_kappa")>>kappa;
  haveSetup();
  cout<<"GLens set up with:\n\tintegrate=";
  if(use_integrate)cout<<"true\n\tGL_int_tol="<<GL_int_tol<<"\n\tkappa="<<kappa<<endl;
  else cout<<"false"<<endl;
  nativeSpace=stateSpace(0);
  setPrior(new sampleable_probability_function(&nativeSpace));//dummy
};


//
// ******************************************************************
// GLensBinary routines *********************************************
// ******************************************************************
//

GLensBinary::GLensBinary(double q,double L,double phi0):q(q),L(L),phi0(phi0),sin_phi0(sin(phi0)),cos_phi0(cos(phi0)){
  typestring="GLens";
  option_name="BinaryLens";
  option_info="Fixed binary point-mass lens";
  NimageMax=5;
  nu=1/(1+q);
  rWide=5;
  do_remap_q=false;
  q_ref=0;
  idx_q=idx_L=idx_phi0=-1;
};

void GLensBinary::setup(){
  set_integrate(!optSet("GL_poly")&&!optSet("poly"));
  *optValue("GL_int_tol")>>GL_int_tol;
  *optValue("GL_int_mag_limit")>>GL_int_mag_limit;
  *optValue("GL_int_kappa")>>kappa;
  *optValue("GLB_rWide")>>rWide;
  haveSetup();
  cout<<"GLens set up with:\n\tintegrate=";
  if(use_integrate)cout<<"true\n\tGL_int_tol="<<GL_int_tol<<"\n\tkappa="<<kappa<<endl;
  else cout<<"false"<<endl;
  if(optSet("remap_q")){
    double q0_val;
    *optValue("q0")>>q0_val;
    remap_q(q0_val);
  }
  GLens::setup();
  //set nativeSpace
  stateSpace space(3);
  string names[] =                                      {"logq","logL","phi0"};
  const int uni=mixed_dist_product::uniform, gauss=mixed_dist_product::gaussian, pol=mixed_dist_product::polar; 
  valarray<double>    centers((initializer_list<double>){   0.0,   0.0,  M_PI});
  valarray<double> halfwidths((initializer_list<double>){   4.0,   1.0,  M_PI});
  valarray<int>         types((initializer_list<int>)   {   uni, gauss,   uni});
  if(do_remap_q)names[3]="s(1+q)";
  space.set_bound(2,boundary(boundary::wrap,boundary::wrap,0,2*M_PI));//set 2-pi-wrapped space for phi0.
  space.set_names(names);  
  nativeSpace=space;
  if(optSet("GLB_gauss_q"))types[0]=gauss;
  if(do_remap_q){
    double qq=2.0/(q_ref+1.0);
    double ds=0.5/(1.0+qq*qq); //ds=(1-s(q=1))/2
    centers[0]=1.0-ds;
    halfwidths[0]=ds;          //ie range=[s(q=1),s(q=inf)=1.0]
    types[0]=uni;
  }
  setPrior(new mixed_dist_product(&nativeSpace,types,centers,halfwidths));
};

Point GLensBinary::map(const Point &p){
  ldouble x=p.x,y=p.y,x1=x-ldouble(L)/2.0L,x2=x+ldouble(L)/2.0L,r1sq=x1*x1+y*y,r2sq=x2*x2+y*y;
  ldouble c1=(1.0L-nu)/r1sq,c2=nu/r2sq;
  return Point(x-x1*c1-x2*c2,y-y*(c1+c2));
};

vector<Point> GLensBinary::invmap(const Point &p){
  const double rTest=1.1*rWide;
  double r2=p.x*p.x+p.y*p.y;
  if(testWide(p,1.0)){
    if(debug||inv_test_mode&&debugint){
      debug=true;
      cout<<"wide"<<endl;
    }
    vector<Point>thWB= invmapWideBinary(p);
    //if fails to converge (rare) revert to WittMao:
    if(thWB.size()==0){
      //cout<<"WideBinary failed to converge"<<endl;
      have_saved_soln=false;//Can't rely on save soln when jumping from WideBinary
      return invmapWittMao(p);
    }
    if(inv_test_mode&&r2<rTest*rTest){
      if(debug||debugint)cout<<"wide-test"<<endl;
      vector<Point>thWM= invmapWittMao(p);
      double mag_WB=mag(thWB),mag_WM=mag(thWM);
      if(abs(mag_WB-mag_WM)/mag_WM>1e-6){//the two methods don't agree.
	if(debug||debugint)cout<<"wide-test FAILED"<<endl;
	double TOL=LEADTOL*LEADTOL*(100000000+p.x*p.x+p.y*p.y+4.0/L/L);
	cout.precision(15);
	cout<<"\nInversion methods disagree: magWB="<<mag_WB<<" magWM="<<mag_WM<<" at ("<<p.x<<","<<p.y<<")"<<endl;
	cout<<"WittMaoCalc:"<<endl;
	debug=true;invmapWittMao(p);debug=false;
	cout<<"----"<<endl;
	for(int i=0;i<max(thWM.size(),thWB.size());i++){
	  if(i<thWB.size()){
	    Point th=thWB[i];
	    Point beta=map(th);
	    double dx=beta.x-p.x,dy=beta.y-p.y;
	    double dbx=th.x-p.x,dby=th.y-p.y;
	    cout<<"WB("<<i<<"): "<<th.x<<" "<<th.y<<" "<<mag(th)<<" : ("<<beta.x<<"-"<<p.x<<","<<beta.y<<"-"<<p.y<<")^2 "<<(dx*dx+dy*dy<TOL*(1+dbx*dbx+dby*dby)?" < ":"!< ")<<TOL*(1+dbx*dbx+dby*dby)<<endl;
	  } else {
	    cout<<"WB(---)"<<endl;
	  }
	  if(i<thWM.size()){
	    Point th=thWM[i];
	    Point beta=map(th);
	    double dx=beta.x-p.x,dy=beta.y-p.y;
	    double dbx=th.x-p.x,dby=th.y-p.y;
	    cout<<"WM("<<i<<"): "<<th.x<<" "<<th.y<<" "<<mag(th)<<" : ("<<beta.x<<"-"<<p.x<<","<<beta.y<<"-"<<p.y<<")^2 "<<(dx*dx+dy*dy<TOL*(1+dbx*dbx+dby*dby)?" < ":"!< ")<<TOL*(1+dbx*dbx+dby*dby)<<endl;
	  } else {
	    cout<<"WM(---)"<<endl;
	  }
	}
      }	
    }
    return thWB; 
  } 
  else {
    //if(inv_test_mode&&debugint){
    if(inv_test_mode){
      debug=true;
      cout<<"not wide"<<endl;
    }
    return invmapWittMao(p);
  }
};

///Inverse lense map in the wide-binary limit.
///
///When L>>1 (ie than Einstein angular radius) then we can pursue and interative solution in
///which the beta distance to (all-but) one of the lens objects can be considered far.  Which
///lens may be close is specified by (the sign of) iwhich.  Then the calculation proceeds by a Newton's method
///approach with the zeroth order solution being an exact single-lens solution with the distant
///lens object ignored.  Subsequent terms include an estimated contribution from the distant
///lens object. 
///
///Let \f$\vec p=p_{near}\f$ indicate the Einstein-scaled angular location of the possibly near lens object and
///define \f$\zeta=\beta_x-p_x + i(\beta_x-p_x)\f$
///We will solve for: \f$z=\theta_x -p_x + i(\theta_y-p_y)\f$ iteratevely. At each iteration we solve:
/// \f[
///    z\approx\zeta+\frac{\nu_{near}}{z^*}+\epsilon
/// \f]
/// where, \f$\epsilon\f$ represents the lens potential contribution from the distant lens object. 
/// For the zeroth iteration, we take \f$\epsilon=0\f$ and subsequently \f$\epsilon=\frac{\nu_{far}}{z+c}\f$
/// using the most recent estimate for \f$z\f$ and \f$c=p_{x,far}-p{x,near}=\pm L\f$.
/// At each iteration the solution is:
/// \f[
///    |z|\approx\frac{|\zeta+\epsilon|}2\left(\sqrt{1+\frac{4\nu_{near}}{|\zeta+\epsilon|^2}}\pm1\right)
/// \f]
/// with the complex argument given such that \f$(\zeta+\epsilon)z^*\f$ is real.
/// As usual for a single lens there two image solutions, inside and outside the Einstein ring. 
/// These are selected above by the indicated choice of sign.  We iterate
/// separately a series of approximate solutions with either sign.
/// Where this approximation is appropriate, there will be one additional solution is near the other lens.
/// Note that it is straightforward to generalize to multiple lenses by changing the definition of \f$\epsilon\f$
/// to include contrinbutions from additional distant lenses.
///
vector<Point> GLensBinary::invmapWideBinary(const Point &p){
  const int maxIter=1000;
  //const int maxIter=20;
  //Lens equation:
  //double x=p.x,y=p.y,x1=x-L/2,x2=x+L/2,r1sq=x1*x1+y*y,r2sq=x2*x2+y*y;
  //double a,b,c1=(1-nu)/r1sq,c2=nu/r2sq;
  //return Point(x-x1*c1-x2*c2,y-y*(c1+c2));
  //pick dominant lens:
#ifdef USE_KIND_16
  typedef long double double_type;
#else
  typedef double double_type;
#endif
  double_type xL=p.x,yL=p.y,x1=xL-(double_type)L/2.0L,x2=xL+(double_type)L/2.0L,r1sq=x1*x1+yL*yL,r2sq=x2*x2+yL*yL;
  double_type nu_neg=(double_type)nu,nu_pos=1.0L-nu_neg,cpos=nu_pos*nu_pos/r1sq,cneg=nu_neg*nu_neg/r2sq;
  double_type nu_n,nu_f,c;
  vector<Point>result;
  if(cpos<cneg){//close (dominant) point is one x<0 half-plane
    //cout<<"neg-dominant"<<endl;
    c=L;
    nu_n=nu_neg;
    nu_f=nu_pos;
  } else {
    //cout<<"pos-dominant"<<endl;
    c=-(double_type)L;
    nu_n=nu_pos;
    nu_f=nu_neg;
  }
  complex<double_type> zp,zm,zf,zeta,ep,em,ef;
  zeta=complex<double_type>(p.x+c/2.0L,p.y);
  ep=em=ef=complex<double_type>(0,0);
  double err=1,relerr=1;
  int iter=0;
  //For initialization we consider two options.  Initialize with the single lens solution, the nominal approach described above,
  //or, when we already have a good solution, (A) initialize with the last-found solution.
  //  th[0]=Point(real(zp)-c/2.0L,imag(zp))   -->   zp = th[0].x + c/2 + I th[0].y
  //  th[1]=Point(real(zm)-c/2.0L,imag(zm))   -->   zp = th[1].x + c/2 + I th[1].y
  //  th[2](Point(real(zf)+c/2.0L,imag(zf))   -->   zp = th[2].x - c/2 + I th[2].y
  //  then,
  //  ep=ep(zp)
  //  em=em(zm)
  //  ef=ef(zf)
  //
  //  Or, alternatively, (B) ep=em=ef=0;
  // We run with the result with the smaller residual.  This also should avoid problems when there are changes in the root order,...
  //First the zero case, which is just one iteration of the main loop with ep=em=ef=0.
  //cout<<" Wide Binary x y = "<<xL<<" "<<yL<<"  c="<<c<<" nu="<<nu_n<<endl;
  if(save_thetas_wide){//This should be equivalent to the nominal result, but there are at least numerical differences.  Needs investigation...
    double p2c2=p.x*p.x+p.y*p.y+c*c;
    double pc=c*p.x;//note ym2=yp2=p2+pc,yf2=p2-pc;
    double_type root=sqrt(1.0L+4.0L*nu_n/(p2c2+pc));
    double_type rootf=sqrt(1.0L+4.0L*nu_f/(p2c2-pc));
    zp=zeta*(double_type)((1.0L+root)/2.0L);
    zm=zeta*(double_type)((1.0L-root)/2.0L);
    zf=(zeta-c)*(double_type)((1.0L-rootf)/2.0L);
    ep=nu_f/conj(zp-c);
    em=nu_f/conj(zm-c);
    ef=nu_n/conj(zf+c);
    //err=abs(ep)+abs(em)+abs(ef);
    err=abs(zp-zeta-nu_n/conj(zp)-ep);//comput residual of lens eq.
    err+=abs(zm-zeta-nu_n/conj(zm)-em);
    err+=abs(zf+c-zeta-nu_f/conj(zf)-ef);
    iter=1;
    //if(verbose) cout<<"save_thetas_wide p=*"<<p.x<<","<<p.y<<"):ep,em,ef"<<ep<<","<<em<<","<<ef<<endl;
  }
  if(save_thetas_wide and have_saved_soln and theta_save.size()==3){
    zp=complex<double_type>(theta_save[0].x+c/2.0L,theta_save[0].y);  //Need to change from saved_roots to theta_save...
    zm=complex<double_type>(theta_save[1].x+c/2.0L,theta_save[1].y);
    zf=complex<double_type>(theta_save[2].x-c/2.0L,theta_save[2].y);
    //we borrow the "old" variables for this.
    complex<double_type>epsave=nu_f/conj(zp-c);
    complex<double_type>emsave=nu_f/conj(zm-c);
    complex<double_type>efsave=nu_n/conj(zf+c);
    cout<<"          ep0="<<zp-zeta-nu_n/conj(zp)<<endl;
    cout<<"          em0="<<zm-zeta-nu_n/conj(zm)<<endl;
    cout<<"          ef0="<<zm+c-zeta-nu_f/conj(zf)<<endl;
    cout<<"  zp="<<zp<<"  ep="<<epsave<<endl;
    cout<<"  zm="<<zm<<"  em="<<emsave<<endl;
    cout<<"  zf="<<zf<<"  ef="<<efsave<<endl;
    double_type errsave=abs(zp-zeta-nu_n/conj(zp)-epsave);
    errsave+=abs(zm-zeta-nu_n/conj(zm)-emsave);
    errsave+=abs(zf+c-zeta-nu_f/conj(zf)-efsave);
    //And this is the test:
    cout<<" naive err="<<err<<"  save err="<<errsave<<endl;
    if(errsave<err){
      ep=epsave;
      em=emsave;
      ef=efsave;
      err=errsave;
    }
    cout<<"save_thetas_wide (chose):ep,em,ef"<<ep<<","<<em<<","<<ef<<endl;
    iter=1;
  }//now we have our preferred choice of ep/em/ef and we are ready for the main loop.

  bool fail=false;
  while(err>epsTOL&&relerr>epsTOL){
    //cout<<"WideBinary: iter="<<iter<<"\n          err="<<err<<"  relerr="<<relerr<<endl;
    iter++;
    if(iter>maxIter){
      if(debug)cout<<"invmapWideBinary maxIter reached: Failing."<<endl;
      return result;
    }
    //cout<<" err="<<err<<endl;
    complex<double_type>epold=ep,emold=em,efold=ef;
    complex<double_type>yp=zeta+ep,ym=zeta+em;
    complex<double_type> yf=zeta-c+ef;
    double_type yp2=real(yp*conj(yp));
    double_type ym2=real(ym*conj(ym));
    double_type yf2=real(yf*conj(yf));
    double_type zpfac=(sqrt(1.0L+4.0L*nu_n/yp2)+1.0L)/2.0L;
    double_type zmfac=-(sqrt(1.0L+4.0L*nu_n/ym2)-1.0L)/2.0L;
    double_type zffac=-(sqrt(1.0L+4.0L*nu_f/yf2)-1.0L)/2.0L;
    zp=yp*zpfac;
    zm=ym*zmfac;
    zf=yf*zffac;
    //if(debug){
    // cout<<"  zpfac="<<zpfac<<"  yp2="<<yp2<<" c="<<c<<endl;
    // cout<<"  zmfac="<<zmfac<<"  ym2="<<ym2<<" c="<<c<<endl;
    // cout<<"  zffac="<<zffac<<"  yf2="<<yf2<<" -c="<<-c<<endl;
    //}
    ep=nu_f/conj(zp-c);
    em=nu_f/conj(zm-c);
    ef=nu_n/conj(zf+c);
    err=abs(ep-epold)+abs(em-emold)+abs(ef-efold);
    double_type zmean=abs(zp)+abs(zm)+abs(zf);
    relerr=abs((ep-epold)*zp*zp/zmean/zmean)+abs((em-emold)*zm*zm/zmean/zmean)+abs((ef-efold)*zf*zf/zmean/zmean);
    if(debug){
      cout<<"  zp="<<zp<<"  ep="<<ep<<" D="<<ep-epold<<endl;
      cout<<"  zm="<<zm<<"  em="<<em<<" D="<<em-emold<<endl;
      cout<<"  zf="<<zf<<"  ef="<<ef<<" D="<<ef-efold<<endl;
      cout<<" err,relerr="<<err<<","<<relerr<<" vs "<<epsTOL<<endl;
      cout<<"  mp="<<mag(Point(real(zp)-c/2.0L,imag(zp)))
	  <<"  mm="<<mag(Point(real(zm)-c/2.0L,imag(zm)))
	  <<"  mf="<<mag(Point(real(zf)-c/2.0L,imag(zf)))<<endl;
    }
  }
  //Should we order the roots for consistency with WittMao results??? 
  result.push_back(Point(real(zp)-c/2.0L,imag(zp)));
  result.push_back(Point(real(zm)-c/2.0L,imag(zm)));
  result.push_back(Point(real(zf)+c/2.0L,imag(zf)));
  //perhaps save the result.
  if(save_thetas_wide ){
    theta_save=result;
    have_saved_soln=true;
  } else {
    have_saved_soln=false;
  }
  return result;
};

vector<Point> GLensBinary::invmapWittMao(const Point &p){
  //Here quantities are like WittMao95, but with:
  // z         ->  z*z1
  // 2m        -> M*z1^2  ; but in our normalization 2 m = nu+(1-nu) = 1
  // 2 Delta m -> DM*z1^2 ; in our normalization 2 Delta m = nu - (1-nu) = 2*nu-1
  // zeta      -> B*z1
  const complex<double> cplx_i=complex<double>(0,1),cplx_1=1;
  double z1=L/2;
  double z12=z1*z1;
  double M=1/z12;
  double DM=M*(2*nu-1);
  complex<double> B=(p.x+cplx_i*p.y)/z1,Bb=conj(B);
  complex<double> u,v,w,wb;
  complex<double> c[7],roots[6];
  int nroots=5;
  bool z1scaled=false;
  //if(z1>LEADTOL){    //scaled with z->z*z1,ck->ck*z1^-k
  if(z1>1){
    z1scaled=true;//scaled with z->z*z1,ck->ck*z1^-k
    u=DM*B+M;
    w=M*Bb+DM;
    wb=conj(w);
    v=wb+Bb;//M*B+DM+Bb
    c[5]=1.0-Bb*Bb;
    c[4]=-B*c[5]-w;
    c[3]=2.0*v*Bb-2.0;
    c[2]=M*wb-2.0*(Bb*u+c[4]);
    c[1]=1.0-v*v-M*M*conj(c[5]);
    c[0]=(DM+2.0*Bb)*u+c[4];
  } 
  else if(true) {    //scaled with z->z*z1,ck->ck*z1^-k
    B=(p.x+cplx_i*p.y);Bb=conj(B);   //=B1*z1     
    M=1;
    DM=2*nu-1;            //=(DM*z1^2 above)
    u=DM*B+z1;           //(=u*z1^3 above)
    w=Bb+DM*z1;        //(=w*z1^3 above)
    wb=conj(w);
    v=wb+Bb*z12;         //(=v*z1^3 above)
    c[5]=z12-Bb*Bb;                         //c5*z1^2
    c[4]=-B*c[5]-w;                         //c4*z1^3
    c[3]=2.0*v*Bb-2.0*z12*z12;              //c3*z1^4
    c[2]=wb-2.0*(Bb*u*z1+c[4]*z12);         //c2*z1^5
    c[1]=z12*z12*z12-v*v-conj(c[5]);        //c1*z1^6
    c[0]=((DM+2.0*Bb*z1)*u+c[4]*z12)*z12;//c0*z1^7
  } 
  else { //handle degenerate (single point lens) case without z1 scalingelse { //handle degenerate (single point lens) case without z1 scaling
    B=(p.x+cplx_i*p.y);Bb=conj(B);
    M=1;
    DM=2*nu-1;
    c[5]=0;c[4]=0;
    c[3]=-Bb*Bb;
    c[2]=-B*c[3]-Bb;
    c[1]=2.0*B*Bb;
    c[0]=B;
    nroots=3;
  }
  //Test for effectively lower order; 
  //If the leading order polynomial coefficients are effectively zero, then the solve will fail.
  double c_lower=0;
  for(int k=0;k<nroots;k++)c_lower+=abs(real(c[k]))+abs(imag(c[k]));
  for(int i=nroots;i>1;i--){
    double val=(abs(real(c[i]))+abs(imag(c[i])))/c_lower;
    if(debug)cout<<"val["<<i<<"]= "<<val<<" ?> "<<LEADTOL<<endl;
    if(val>LEADTOL)break;
    //break; //debug  Seems to work better without throwing out terms here  WHY?
    //The original reason for this was failure of the polynomial solution when the
    //leading order coeficient vanishes.
    //I don't think I know how it behaves when the coeficient is just very small, though it makes sense that
    //there must at least be an underflow issue at some point.
    //I think, at the time, I wasn't testing regions of parameter space with such extreme values..
    nroots--;
    c_lower-=abs(real(c[nroots]))+abs(imag(c[nroots]));
  }
  //Debug
  if(debug){
    cout<<"nu="<<nu<<" z1="<<z1<<" M="<<M<<" DM="<<DM<<" B= "<<B<<" u="<<u<<" w="<<w<<" v="<<v<<endl;
    cout<<"nroots="<<nroots<<endl;
    for( int i=0;i<=5;i++)
      cout<<"c["<<i<<"]="<<c[i]<<(i>nroots?"**":"")<<endl;
  }

  bool roots_changed =true;//(this isn't used but compiler may be concerned about an uninitialized value) 
  bool polish_only = false;
  if(nroots==5 and save_thetas_poly and have_saved_soln and theta_save.size()==5){
    //for(int i=0;i<5;i++)roots[i]=saved_roots[i];
    for(int i=0;i<5;i++)roots[i]=complex<double>(theta_save[i].x,theta_save[i].y);
    polish_only=true;
    if(verbose){
      cout<<"using saved roots: p=("<<p.x<<","<<p.y<<")"<<endl;
      for(int i=0;i<5;i++)cout<<"  "<<i<<": "<<roots[i]<<endl;
    }
  } else if(save_thetas_poly and verbose) cout<<"   no saved roots: p=("<<p.x<<","<<p.y<<")"<<endl;

  if(nroots==5)cmplx_roots_5(roots, roots_changed, c, polish_only);
  else cmplx_roots_gen(roots, c, nroots,true,false);
  if(save_thetas_poly and nroots==5){
    theta_save.resize(5);
    for(int i=0;i<5;i++)theta_save[i]=Point(real(roots[i]),imag(roots[i]));
    have_saved_soln=true;
  } else {
    have_saved_soln=false;
  }
  
  if(nroots==4&&fix_nr4_roots){//try to make linear correction to root values
    for( int i=0;i<nroots;i++){
      complex<__float128> dP4=0.0;
      complex<__float128> z=roots[i];
      for(int k=1;k<=4;k++){
	dP4+=c[k]*(double)k;
	dP4/=z;
      };
      //Result is dP4 = dP/dz(z_k) / z_k^4
      //dP4=((((c1/z+2*c2)/z+3*c3)/z+4*c4)/z
      //   =(c1+2*c2*z+3*c3*z^2+4*z4+z^4)/z^4
      complex<double> double_dP4;double_dP4=dP4;
      complex<double> delta=-c[5]/double_dP4;
      if(debug)cout<<"dP4="<<double_dP4<<" delta="<<delta<<endl;
      ///We can derive this following expression by setting P5(x+eps)-P4(x)=0, then solve liniarly for eps
      ///For abs(delta)<<0.2 and abs(delta)>>0.2 the result approximates that in the comment above, but all values other than
      ///delta=-0.2 are allowed.  In this case, the 0.2 arises because n=5, not arbitrarily.
      roots[i]*=(1.0+cplx_1/(5.0+cplx_1/delta));
    }
  }
  //For debugging, test the results;
  if(debug){
    cout<<"complex poly test:"<<endl;
    for( int i=0;i<nroots;i++){
      complex<__float128> res=0.0;
      complex<__float128> z=roots[i];
      for(int k=5;k>=0;k--){
	res*=z;
	res+=c[k];
      }
      complex<long double> lres,lz;
      lres=res;lz=z;
      cout<<i<<": "<<lz<<"->"<<lres<<endl;
    }
  }
  
  vector<Point> result;
  const double TOL=LEADTOL*LEADTOL;
  for(int i=0;i<nroots;i++){
    Point newp=Point(0,0);
    if(z1scaled)
      newp=Point(real(roots[i])*z1,imag(roots[i])*z1);
    else 
      newp=Point(real(roots[i]),imag(roots[i]));
    //Test solutions: Could also try keeping those with err/(1+B*Bb)<TOL say
    Point btheta=map(newp);
    double dx=btheta.x-p.x,dy=btheta.y-p.y;
    //double dbx=newp.x-p.x,dby=newp.y-p.y;
    double x=newp.x,y=newp.y,x1=x-L/2,x2=x+L/2,r1sq=x1*x1+y*y,r2sq=x2*x2+y*y;
    double c1=(1-nu)/r1sq,c2=nu/r2sq;
    if(debug){
      cout<<"testing: "<<roots[i]<<" -> |("<<btheta.x<<"-"<<p.x<<","<<btheta.y<<"-"<<p.y<<")|="<<sqrt(dx*dx+dy*dy)<<" "<<(dx*dx+dy*dy<TOL*(1+c1+c2)?" < ":"!< ")<<LEADTOL*sqrt(1+c1+c2)<<"="<<LEADTOL<<"*√(1+"<<c1<<"+"<<c2<<")"<<endl;
    }
    if(dx*dx+dy*dy<TOL*(1+c1+c2))result.push_back(newp);      //RHS is squared estimate in propagating error of LEADTOL in root through map()
    //For now we just adopt the ordering from the SG code.  Might change to something else if needed...
  };
  if(debug)cout<<"Found "<<result.size()<<" images."<<endl;

  return result;
};

double GLensBinary::mag(const Point &p){
  double x=p.x,y=p.y,x1=x-L/2,x2=x+L/2,r1sq=x1*x1+y*y,r2sq=x2*x2+y*y;
  //cout<<"x,y,r1sq,r2sq:"<<x<<" "<<y<<" "<<r1sq<<" "<<r2sq<<endl;
  double m1=(1-nu), m2=nu, dEr4=m1*r2sq-m2*r1sq;
  double cosr2=x1*x2+y*y,cos2r4=cosr2*cosr2;
  double r4=r1sq*r2sq,r8=r4*r4;
  double invmuR8=r8-dEr4*dEr4-4*m1*m2*cos2r4;
  double mg=0;
  if(r8>0)mg=r8/invmuR8;
  //cout<<"mg="<<mg<<endl;
  return mg;
};

double GLensBinary::jac(const Point &p,double &j00,double &j01,double &j10,double &j11){
  double x=p.x,y=p.y,x1=x-L/2,x2=x+L/2,y2=y*y,r1sq=x1*x1+y2,r2sq=x2*x2+y2;
  double E1=(1-nu)/r1sq, E2=nu/r2sq,dE=E1-E2,E=E1+E2;
  double cos2=x1*x2+y2;cos2*=cos2/r1sq/r2sq;
  double invmu=1-dE*dE-4*E1*E2*cos2;
  double mu=1/invmu;
  double E1w=2*E1/r1sq,E2w=2*E2/r2sq;
  double f=(E1w+E2w)*y2 - E;
  j10 = j01 = (E1w*x1+E2w*x2)*y;
  j00       = 1-f;
  j11       = 1+f;
  return mu;
};

///The inverse jacobian can be finite when the jacobian is not so we compute it directly
double GLensBinary::invjac(const Point &p,double &ij00,double &ij01,double &ij10,double &ij11){
  double x=p.x,y=p.y,x1=x-L/2,x2=x+L/2,y2=y*y,r1sq=x1*x1+y2,r2sq=x2*x2+y2;
  double E1r4=(1-nu)*r2sq, E2r4=nu*r1sq,dEr4=E1r4-E2r4,Er4=E1r4+E2r4;
  double cosr2=x1*x2+y2,cos2r4=cosr2*cosr2;
  double r4=r1sq*r2sq,r8=r4*r4;
  double invmuR8=r8-dEr4*dEr4-4*nu*(1-nu)*cos2r4;
  double mu=r8/invmuR8;
  double E1wr8=2*E1r4*r2sq,E2wr8=2*E2r4*r1sq;
  double fr8=(E1wr8+E2wr8)*y2 - Er4*r4;
  ij10 = ij01 = -(E1wr8*x1+E2wr8*x2)*y/invmuR8;
  ij00       = (r8+fr8)/invmuR8;
  ij11       = (r8-fr8)/invmuR8;
  return mu;
};


