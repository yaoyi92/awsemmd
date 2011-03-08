/* ----------------------------------------------------------------------
Copyright (2010) Aram Davtyan and Garegin Papoian

Papoian's Group, University of Maryland at Collage Park
http://papoian.chem.umd.edu/

Last Update: 12/01/2010
------------------------------------------------------------------------- */

#include "math.h"
#include "string.h"
#include "stdlib.h"
#include "fix_qbias.h"
#include "atom.h"
#include "timer.h"
#include "update.h"
#include "respa.h"
#include "error.h"
#include "group.h"
#include "domain.h"
#include "fstream.h"

#include <iostream.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

using namespace LAMMPS_NS;

/* ---------------------------------------------------------------------- */

FixQBias::FixQBias(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
	if (narg != 4) error->all("Illegal fix qbias command");

	char force_file_name[] = "forcesQ.dat";
	fout = fopen(force_file_name,"w");
	
	scalar_flag = 1;
	vector_flag = 1;
	size_vector = 3;
//	scalar_vector_freq = 1;
	extscalar = 1;
	extvector = 1;

	force_flag = 0;
	n = (int)(group->count(igroup)+1e-12);
	foriginal[0] = foriginal[1] = foriginal[2] = foriginal[3] = 0.0;

	allocated = false;
	allocate();

	qbias_flag = 0;
	qbias_exp_flag = 0;
	epsilon = 1.0;

	int i, j;
	char varsection[30];
	ifstream in(arg[3]);
	if (!in) error->all("Coefficient file was not found!");
	while (!in.eof()) {
		in >> varsection;
		if (strcmp(varsection, "[Epsilon]")==0) {
			in >> epsilon;
		} else if (strcmp(varsection, "[QBias]")==0) {
			qbias_flag = 1;
			in >> k_qbias;
			in >> q0;
			in >> l;
			in >> sigma;
		}  else if (strcmp(varsection, "[QBias_Exp]")==0) {
			qbias_exp_flag = 1;
			in >> k_qbias;
			in >> q0;
			in >> l;
			in >> sigma_exp;
		}
	}
	in.close();

	ifstream in_rnative("rnative.dat");
	for (i=0;i<n;++i)
		for (j=0;j<n;++j)
			in_rnative >> rN[i][j];
	in_rnative.close();

	for (i=0;i<n;i++) sigma_sq[i] = Sigma(i)*Sigma(i);
	for (i=0;i<n;i++) fprintf(fout, "sigma_sq[%d]=%f:\n",i, sigma_sq[i]);

	x = atom->x;
	f = atom->f;
	image = atom->image;
	prd[0] = domain->xprd;
	prd[1] = domain->yprd;
	prd[2] = domain->zprd;
	half_prd[0] = prd[0]/2;
	half_prd[1] = prd[1]/2;
	half_prd[2] = prd[2]/2; 
	periodicity = domain->periodicity;

	Step = 0;
	sStep=0, eStep=0;
	ifstream in_rs("record_steps");
	in_rs >> sStep >> eStep;
	in_rs.close();
}

/* ---------------------------------------------------------------------- */

FixQBias::~FixQBias()
{
	if (allocated) {
		for (int i=0;i<n;i++) {
			delete [] r[i];
			delete [] rN[i];
			delete [] q[i];

			delete [] xca[i];
		}

		delete [] r;
		delete [] rN;
		delete [] q;

		delete [] alpha_carbons;
		delete [] xca;
		delete [] res_no;
		delete [] res_info;

		delete [] sigma_sq;
	}
}

/* ---------------------------------------------------------------------- */
inline int MIN(int a, int b)
{
	if ((a<b && a!=-1) || b==-1) return a;
	else return b;
}

inline int MAX(int a, int b)
{
	if ((a>b && a!=-1) || b==-1) return a;
	else return b;
}

inline bool FixQBias::isFirst(int index)
{
	if (res_no[index]==1) return true;
	return false;
}

inline bool FixQBias::isLast(int index)
{
	if (res_no[index]==n) return true;
	return false;
}

int FixQBias::Tag(int index) {
	if (index==-1) return -1;
	return atom->tag[index];
}

inline void FixQBias::Construct_Computational_Arrays()
{
	int *mask = atom->mask;
	int nlocal = atom->nlocal;
	int nall = atom->nlocal + atom->nghost;
	int *mol_tag = atom->molecule;

	int i, j, js;

	// Creating index arrays for Alpha_Carbons
	nn = 0;
	int last = 0;
	for (i = 0; i < n; ++i) {
		int min = -1, jm = -1;
		for (int j = 0; j < nall; ++j) {
			if (i==0 && mol_tag[j]<=0)
				error->all("Molecular tag must be positive in fix qbias");
			
			if ( (mask[j] & groupbit) && mol_tag[j]>last ) {
				if (mol_tag[j]<min || min==-1) {
					min = mol_tag[j];
					jm = j;
				}
			}
		}
		
		if (min==-1) break;

		alpha_carbons[nn] = jm;
		res_no[nn] = min;
		last = min;
		nn++;	
	}

	/*if (Step>=sStep && Step<=eStep) {
		fprintf(fout, "\n\n");
		for (i = 0; i < nn; ++i) {
			fprintf(fout, "%d ", res_no[i]);
		}
		fprintf(fout, "\n\n");
	}*/

	int nMinNeighbours = 3;
	int iLastLocal = -1;
	int lastResNo = -1;
	int lastResType = NONE;
	int nlastType = 0;

	// Checking sequance and marking residues
	for (i = 0; i < nn; ++i) {
		if (lastResNo!=-1 && lastResNo!=res_no[i]-1) {
			if (lastResType==LOCAL && res_no[i]!=n)
				error->all("Missing neighbor atoms in fix qbias (code: 001)");
			if (lastResType==GHOST) {
				if (iLastLocal!=-1 && i-nMinNeighbours<=iLastLocal)
					error->all("Missing neighbor atoms in fix qbias (code: 002)");
//				else {
//					js = i - nlastType;
//					if (iLastLocal!=-1) js = MAX(js, iLastLocal + nMinNeighbours + 1);
//					for (j=js;j<i;++j) res_info[j] = OFF;
//				}
			}
			
			iLastLocal = -1;
			lastResNo = -1;
			lastResType = NONE;
			nlastType = 0;
		}

		if (alpha_carbons[i]!=-1) {
			if (alpha_carbons[i]<nlocal) {
				if ( lastResType==OFF || (lastResType==GHOST && nlastType<nMinNeighbours && nlastType!=res_no[i]-1) ) {
					error->all("Missing neighbor atoms in fix qbias  (code: 003)");
				}
				iLastLocal = i;
				res_info[i] = LOCAL;
			} else {
//				if ( lastResType==GHOST && nlastType>=nMinNeighbours && (iLastLocal==-1 || i-2*nMinNeighbours-iLastLocal>=0) ) {
//					res_info[i-nMinNeighbours] = OFF;
//					nlastType = nMinNeighbours-1;
//				}

				res_info[i] = GHOST;
			}
		} else res_info[i] = OFF;

		if (lastResNo == res_no[i]) nlastType++; else nlastType = 0;

		lastResNo = res_no[i];
		lastResType = res_info[i];
	}
	if (lastResType==LOCAL && res_no[nn-1]!=n)
		error->all("Missing neighbor atoms in fix qbias  (code: 004)");
	if (lastResType==GHOST) {
		if (iLastLocal!=-1 && nn-nMinNeighbours<=iLastLocal)
			error->all("Missing neighbor atoms in fix qbias  (code: 005)");
//		else {
//			js = nn - nlastType;
//			if (iLastLocal!=-1) js = MAX(js, iLastLocal + nMinNeighbours + 1);
//			for (j=js;j<nn;++j) res_info[j] = OFF;
//		}
	}

/*	if (Step>=sStep && Step<=eStep) {
		for (i = 0; i < nn; ++i) {
			fprintf(fout, "%d ", res_info[i]);
		}
		fprintf(fout, "\n");
	}*/
}

void FixQBias::allocate()
{
	r = new double*[n];
	rN = new double*[n];
	q = new double*[n];

	sigma_sq = new double[n];

	alpha_carbons = new int[n];
	xca = new double*[n];
	res_no = new int[n];
	res_info = new int[n];

	for (int i = 0; i < n; ++i) {
		r[i] = new double [n];
		rN[i] = new double [n];
		q[i] = new double [n];

		xca[i] = new double [3];
	}
	
	allocated = true;
}

/* ---------------------------------------------------------------------- */

int FixQBias::setmask()
{
	int mask = 0;
	mask |= POST_FORCE;
	mask |= THERMO_ENERGY;
	mask |= POST_FORCE_RESPA;
	mask |= MIN_POST_FORCE;
	return mask;
}

/* ---------------------------------------------------------------------- */

void FixQBias::init()
{
	if (strcmp(update->integrate_style,"respa") == 0)
		nlevels_respa = ((Respa *) update->integrate)->nlevels;
}

/* ---------------------------------------------------------------------- */

void FixQBias::setup(int vflag)
{
	if (strcmp(update->integrate_style,"verlet") == 0)
		post_force(vflag);
	else {
		((Respa *) update->integrate)->copy_flevel_f(nlevels_respa-1);
		post_force_respa(vflag,nlevels_respa-1,0);
		((Respa *) update->integrate)->copy_f_flevel(nlevels_respa-1);
	}
}

/* ---------------------------------------------------------------------- */

void FixQBias::min_setup(int vflag)
{
	post_force(vflag);
}

/* ---------------------------------------------------------------------- */

inline double adotb(double *a, double *b)
{
	return (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]);
}

inline double cross(double *a, double *b, int index)
{
	switch (index) {
		case 0:
			return a[1]*b[2] - a[2]*b[1];
		case 1:
			return a[2]*b[0] - a[0]*b[2];
		case 2:
			return a[0]*b[1] - a[1]*b[0];
	}
	
	return 0;
}

inline double atan2(double y, double x)
{
	if (x==0) {
		if (y>0) return M_PI_2;
		else if (y<0) return -M_PI_2;
		else return NULL;
	} else {
		return atan(y/x) + (x>0 ? 0 : (y>=0 ? M_PI : -M_PI) );
	}
}

inline double FixQBias::PeriodicityCorrection(double d, int i)
{
	if (!periodicity[i]) return d;
	else return ( d > half_prd[i] ? d - prd[i] : (d < -half_prd[i] ? d + prd[i] : d) );
}

double FixQBias::Sigma(int sep)
{
	if (qbias_exp_flag)
		return pow(1+sep, sigma_exp);

	return sigma;
}

void FixQBias::compute_qbias() 
{
	double qsum, a, dx[3], dr, dql1, dql;
	double force, force1;
	int i, j;

	qsum = 0;
	a = 2.0/((n-2)*(n-3));
	for (i=0;i<n;++i) {
		for (j=i+3;j<n;++j) {
			dx[0] = xca[i][0] - xca[j][0];
			dx[1] = xca[i][1] - xca[j][1];
			dx[2] = xca[i][2] - xca[j][2];
			
			r[i][j] = sqrt(dx[0]*dx[0]+dx[1]*dx[1]+dx[2]*dx[2]);
			dr = r[i][j] - rN[i][j];
			q[i][j] = exp(-dr*dr/(2*sigma_sq[j-i]));
			qsum += q[i][j];
		}
	}
	qsum = a*qsum;

//	fprintf(fout, "Q%d=%f:\n",Step, qsum);

	dql1 = pow(qsum-q0, l-1);
	dql = dql1*(qsum-q0);

	foriginal[0] += epsilon*k_qbias*dql;

	force = epsilon*k_qbias*dql1*l*a;

	for (i=0;i<n;++i) {
		for (j=i+3;j<n;++j) {
			dx[0] = xca[i][0] - xca[j][0];
			dx[1] = xca[i][1] - xca[j][1];
			dx[2] = xca[i][2] - xca[j][2];

			dr = r[i][j] - rN[i][j];

			force1 = force*q[i][j]*dr/r[i][j]/sigma_sq[j-i];

			f[alpha_carbons[i]][0] -= -dx[0]*force1;
			f[alpha_carbons[i]][1] -= -dx[1]*force1;
			f[alpha_carbons[i]][2] -= -dx[2]*force1;

			f[alpha_carbons[j]][0] -= dx[0]*force1;
			f[alpha_carbons[j]][1] -= dx[1]*force1;
			f[alpha_carbons[j]][2] -= dx[2]*force1;
		}
	}
}

void FixQBias::out_xyz_and_force(int coord)
{
//	out.precision(12);
	
	fprintf(fout, "%d\n", Step);
	fprintf(fout, "%d%d\n", qbias_flag, qbias_exp_flag);
	fprintf(fout, "Number of atoms %d\n", n);
	fprintf(fout, "Energy: %d\n\n", foriginal[0]);

	int index;	

	if (coord==1) {
		fprintf(fout, "rca = {");
		for (int i=0;i<nn;i++) {
			index = alpha_carbons[i];
			if (index!=-1) {
				fprintf(fout, "{%.8f, %.8f, %.8f}", x[index][0], x[index][1], x[index][2]);
				if (i!=nn-1) fprintf(fout, ",\n");
			}
		}
		fprintf(fout, "};\n\n\n");
	}
	

	fprintf(fout, "fca = {");
	for (int i=0;i<nn;i++) {
		index = alpha_carbons[i];
		if (index!=-1) {
			fprintf(fout, "{%.8f, %.8f, %.8f}", f[index][0], f[index][1], f[index][2]);
			if (i!=nn-1) fprintf(fout, ",\n");

		}
	}
	fprintf(fout, "};\n\n\n\n");
}

void FixQBias::compute()
{
	ntimestep = update->ntimestep;

	Step++;

	if(atom->nlocal==0) return;

	Construct_Computational_Arrays();

	x = atom->x;
        f = atom->f;
        image = atom->image;

	int i, j, xbox, ybox, zbox;
	int i_resno, j_resno;
	
	foriginal[0] = foriginal[1] = foriginal[2] = foriginal[3] = 0.0;
	force_flag = 0;

	for (i=0;i<nn;++i) {
		if (res_info[i]==LOCAL) {
			foriginal[1] += f[alpha_carbons[i]][0];
			foriginal[2] += f[alpha_carbons[i]][1];
			foriginal[3] += f[alpha_carbons[i]][2];
		}

		// Calculating xca Ca atoms coordinates array
		if ( (res_info[i]==LOCAL || res_info[i]==GHOST) ) {
			if (domain->xperiodic) {
				xbox = (image[alpha_carbons[i]] & 1023) - 512;
				xca[i][0] = x[alpha_carbons[i]][0] + xbox*prd[0];
			} else xca[i][0] = x[alpha_carbons[i]][0];
			if (domain->yperiodic) {
				ybox = (image[alpha_carbons[i]] >> 10 & 1023) - 512;
				xca[i][1] = x[alpha_carbons[i]][1] + ybox*prd[1];
			} else xca[i][1] = x[alpha_carbons[i]][1];
			if (domain->zperiodic) {
				zbox = (image[alpha_carbons[i]] >> 20) - 512;
				xca[i][2] = x[alpha_carbons[i]][2] + zbox*prd[2];
			} else xca[i][2] = x[alpha_carbons[i]][2];
		}
	}

/*	for (i=0;i<nn;++i) {
		if (res_info[i]!=LOCAL) continue;

		if (bonds_flag && res_no[i]<=n-1)
			compute_bond(i);

		if (angles_flag && res_no[i]<=n-2)
                        compute_angle(i);

		if (dihedrals_flag && res_no[i]<=n-3)
                        compute_dihedral(i);

		for (j=i+1;j<nn;++j) {
			if (res_info[j]!=LOCAL && res_info[j]!=GHOST) continue;

                        if (contacts_flag && res_no[i]<res_no[j]-3)
                                compute_contact(i, j);
                }
	}*/

	double tmp, tmp2;
	double tmp_time;
	int me,nprocs;
  	MPI_Comm_rank(world,&me);
  	MPI_Comm_size(world,&nprocs);
	if (Step>=sStep && Step<=eStep) {
		fprintf(fout, "At Start %d:\n", nn);
		out_xyz_and_force(1);
	}

	tmp = foriginal[0];
	if (qbias_flag || qbias_exp_flag)
		compute_qbias();
	if ((qbias_flag || qbias_exp_flag) && Step>=sStep && Step<=eStep) {
		fprintf(fout, "Qbias %d:\n", nn);
		fprintf(fout, "Qbias_Energy: %.12f\n", foriginal[0]-tmp);
		out_xyz_and_force();
	}

	if (Step>=sStep && Step<=eStep) {
		fprintf(fout, "All:\n");
		out_xyz_and_force(1);
		fprintf(fout, "\n\n\n");
	}
}

/* ---------------------------------------------------------------------- */

void FixQBias::post_force(int vflag)
{
	compute();
}

/* ---------------------------------------------------------------------- */

void FixQBias::post_force_respa(int vflag, int ilevel, int iloop)
{
	if (ilevel == nlevels_respa-1) post_force(vflag);
}

/* ---------------------------------------------------------------------- */

void FixQBias::min_post_force(int vflag)
{
	post_force(vflag);
}

/* ----------------------------------------------------------------------
	 potential energy of added force
------------------------------------------------------------------------- */

double FixQBias::compute_scalar()
{
	// only sum across procs one time

	if (force_flag == 0) {
		MPI_Allreduce(foriginal,foriginal_all,4,MPI_DOUBLE,MPI_SUM,world);
		force_flag = 1;
	}
	return foriginal_all[0];
}

/* ----------------------------------------------------------------------
	 return components of total force on fix group before force was changed
------------------------------------------------------------------------- */

double FixQBias::compute_vector(int n)
{
	// only sum across procs one time

	if (force_flag == 0) {
		MPI_Allreduce(foriginal,foriginal_all,4,MPI_DOUBLE,MPI_SUM,world);
		force_flag = 1;
	}
	return foriginal_all[n+1];
}