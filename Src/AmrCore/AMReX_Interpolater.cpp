
#include <climits>

#include <AMReX_FArrayBox.H>
#include <AMReX_Geometry.H>
#include <AMReX_Interpolater.H>
#include <AMReX_INTERP_F.H>
#include <AMReX_Interp_C.H>

namespace amrex {

//
// PCInterp, NodeBilinear, and CellConservativeLinear are supported for all dimensions on cpu and gpu.
//
// CellConsertiveProtected only works in 2D and 3D on cpu.
//
// CellBilinear only works in 1D and 2D on cpu.
//
// CellQuadratic only works in 2D and 3D on cpu.
//
// CellConservativeQuartic only works with ref ratio of 2 on cpu
//

//
// CONSTRUCT A GLOBAL OBJECT OF EACH VERSION.
//
PCInterp                  pc_interp;
NodeBilinear              node_bilinear_interp;
CellBilinear              cell_bilinear_interp;
CellQuadratic             quadratic_interp;
CellConservativeLinear    lincc_interp;
CellConservativeLinear    cell_cons_interp(0);
CellConservativeProtected protected_interp;
CellConservativeQuartic   quartic_interp;
CellGaussianProcess       gp_interp; 

Interpolater::~Interpolater () {}

InterpolaterBoxCoarsener
Interpolater::BoxCoarsener (const IntVect& ratio)
{
    return InterpolaterBoxCoarsener(this, ratio);
}

Box
InterpolaterBoxCoarsener::doit (const Box& fine) const
{
    return mapper->CoarseBox(fine, ratio);
}

BoxConverter*
InterpolaterBoxCoarsener::clone () const
{
    return new InterpolaterBoxCoarsener(mapper, ratio);
}

NodeBilinear::~NodeBilinear () {}

Box
NodeBilinear::CoarseBox (const Box& fine,
                         int        ratio)
{
    Box b = amrex::coarsen(fine,ratio);

    for (int i = 0; i < AMREX_SPACEDIM; i++)
    {
        if (b.length(i) < 2)
        {
            //
            // Don't want degenerate boxes.
            //
            b.growHi(i,1);
        }
    }

    return b;
}

Box
NodeBilinear::CoarseBox (const Box&     fine,
                         const IntVect& ratio)
{
    Box b = amrex::coarsen(fine,ratio);

    for (int i = 0; i < AMREX_SPACEDIM; i++)
    {
        if (b.length(i) < 2)
        {
            //
            // Don't want degenerate boxes.
            //
            b.growHi(i,1);
        }
    }

    return b;
}

void
NodeBilinear::interp (const FArrayBox&  crse,
                      int               crse_comp,
                      FArrayBox&        fine,
                      int               fine_comp,
                      int               ncomp,
                      const Box&        fine_region,
                      const IntVect&    ratio,
                      const Geometry& /*crse_geom */,
                      const Geometry& /*fine_geom */,
                      Vector<BCRec>&   /*bcr*/,
                      int               /*actual_comp*/,
                      int               /*actual_state*/)
{
    BL_PROFILE("NodeBilinear::interp()");

    FArrayBox const* crsep = &crse;
    FArrayBox* finep = &fine;

    Gpu::LaunchSafeGuard lg(Gpu::isGpuPtr(crsep) && Gpu::isGpuPtr(finep));

    int num_slope  = ncomp*(AMREX_D_TERM(2,*2,*2)-1);
    const Box cslope_bx = amrex::enclosedCells(CoarseBox(fine_region, ratio));
    AsyncFab as_slopefab(cslope_bx, num_slope);
    FArrayBox* slopefab = as_slopefab.fabPtr();

    AMREX_LAUNCH_HOST_DEVICE_LAMBDA (cslope_bx, tbx,
    {
        amrex::nodebilin_slopes(tbx, *slopefab, *crsep, crse_comp, ncomp, ratio);
    });

    AMREX_LAUNCH_HOST_DEVICE_LAMBDA (fine_region, tbx,
    {
        amrex::nodebilin_interp(tbx, *finep, fine_comp, ncomp, *slopefab, *crsep, crse_comp, ratio);
    });
}

CellBilinear::~CellBilinear () {}

Box
CellBilinear::CoarseBox (const Box& fine,
                         int        ratio)
{
    return CoarseBox(fine, ratio*IntVect::TheUnitVector());
}

Box
CellBilinear::CoarseBox (const Box&     fine,
                         const IntVect& ratio)
{
    const int* lo = fine.loVect();
    const int* hi = fine.hiVect();

    Box crse(amrex::coarsen(fine,ratio));
    const int* clo = crse.loVect();
    const int* chi = crse.hiVect();

    for (int i = 0; i < AMREX_SPACEDIM; i++)
    {
        int iratio = ratio[i];
        int hrat   = iratio/2;
        if (lo[i] <  clo[i]*ratio[i] + hrat)
            crse.growLo(i,1);
        if (hi[i] >= chi[i]*ratio[i] + hrat)
            crse.growHi(i,1);
    }
    return crse;
}

void
CellBilinear::interp (const FArrayBox&  crse,
                      int               crse_comp,
                      FArrayBox&        fine,
                      int               fine_comp,
                      int               ncomp,
                      const Box&        fine_region,
                      const IntVect &   ratio,
                      const Geometry& /*crse_geom*/,
                      const Geometry& /*fine_geom*/,
                      Vector<BCRec>&   /*bcr*/,
                      int               actual_comp,
                      int               actual_state)
{
    BL_PROFILE("CellBilinear::interp()");
#if (AMREX_SPACEDIM == 3)
    amrex::Error("interp: not implemented");
#endif
    //
    // Set up to call FORTRAN.
    //
    const int* clo = crse.box().loVect();
    const int* chi = crse.box().hiVect();
    const int* flo = fine.loVect();
    const int* fhi = fine.hiVect();
    const int* lo  = fine_region.loVect();
    const int* hi  = fine_region.hiVect();
    int num_slope  = AMREX_D_TERM(2,*2,*2)-1;
    int len0       = crse.box().length(0);
    int slp_len    = num_slope*len0;

    Vector<Real> slope(slp_len);

    int strp_len = len0*ratio[0];

    Vector<Real> strip(strp_len);

    int strip_lo = ratio[0] * clo[0];
    int strip_hi = ratio[0] * chi[0];

    const Real* cdat  = crse.dataPtr(crse_comp);
    Real*       fdat  = fine.dataPtr(fine_comp);
    const int* ratioV = ratio.getVect();

    amrex_cbinterp (cdat,AMREX_ARLIM(clo),AMREX_ARLIM(chi),AMREX_ARLIM(clo),AMREX_ARLIM(chi),
                   fdat,AMREX_ARLIM(flo),AMREX_ARLIM(fhi),AMREX_ARLIM(lo),AMREX_ARLIM(hi),
                   AMREX_D_DECL(&ratioV[0],&ratioV[1],&ratioV[2]),&ncomp,
                   slope.dataPtr(),&num_slope,strip.dataPtr(),&strip_lo,&strip_hi,
                   &actual_comp,&actual_state);
}

Vector<int>
Interpolater::GetBCArray (const Vector<BCRec>& bcr)
{
    Vector<int> bc(2*AMREX_SPACEDIM*bcr.size());

    for (int n = 0; n < bcr.size(); n++)
    {
        const int* b_rec = bcr[n].vect();

        for (int m = 0; m < 2*AMREX_SPACEDIM; m++)
        {
            bc[2*AMREX_SPACEDIM*n + m] = b_rec[m];
        }
    }

    return bc;
}

CellConservativeLinear::CellConservativeLinear (bool do_linear_limiting_)
{
    do_linear_limiting = do_linear_limiting_;
}

CellConservativeLinear::~CellConservativeLinear ()
{}

Box
CellConservativeLinear::CoarseBox (const Box&     fine,
                                   const IntVect& ratio)
{
    Box crse = amrex::coarsen(fine,ratio);
    crse.grow(1);
    return crse;
}

Box
CellConservativeLinear::CoarseBox (const Box& fine,
                                   int        ratio)
{
    Box crse(amrex::coarsen(fine,ratio));
    crse.grow(1);
    return crse;
}

void
CellConservativeLinear::interp (const FArrayBox& crse,
                                int              crse_comp,
                                FArrayBox&       fine,
                                int              fine_comp,
                                int              ncomp,
                                const Box&       fine_region,
                                const IntVect&   ratio,
                                const Geometry&  crse_geom,
                                const Geometry&  fine_geom,
                                Vector<BCRec>&   bcr,
                                int              /*actual_comp*/,
                                int              /*actual_state*/)
{
    BL_PROFILE("CellConservativeLinear::interp()");
    BL_ASSERT(bcr.size() >= ncomp);

    AMREX_ASSERT(fine.box().contains(fine_region));

    FArrayBox const* crsep = &crse;
    FArrayBox* finep = &fine;

    Gpu::LaunchSafeGuard lg(Gpu::isGpuPtr(crsep) && Gpu::isGpuPtr(finep));

    const Box& crse_region = CoarseBox(fine_region,ratio);
    const Box& cslope_bx = amrex::grow(crse_region,-1);

    AsyncArray<BCRec> async_bcr(bcr.data(), ncomp);
    BCRec* bcrp = async_bcr.data();

    // component of ccfab : slopes for first compoent for x-direction
    //                      slopes for second component for x-direction
    //                      ...
    //                      slopes for last component for x-direction
    //                      slopes for y-direction
    //                      slopes for z-drction
    // then followed by
    //      lin_lim = true : factors (one for all components) for x, y and z-direction
    //      lin_lim = false: min for every component followed by max for every component
    const int ntmp = do_linear_limiting ? (ncomp+1)*AMREX_SPACEDIM : ncomp*(AMREX_SPACEDIM+2);
    AsyncFab as_ccfab(cslope_bx, ntmp);
    FArrayBox* ccfab = as_ccfab.fabPtr();

    const Vector<Real>& vec_voff = amrex::ccinterp_compute_voff(cslope_bx, ratio, crse_geom, fine_geom);

    AsyncArray<Real> async_voff(vec_voff.data(), vec_voff.size());
    Real const* voff = async_voff.data();

    if (do_linear_limiting) {
        AMREX_LAUNCH_HOST_DEVICE_LAMBDA (cslope_bx, tbx,
        {
            amrex::cellconslin_slopes_linlim(tbx, *ccfab, *crsep, crse_comp, ncomp, bcrp);
        });

        AMREX_LAUNCH_HOST_DEVICE_LAMBDA (fine_region, tbx,
        {
            amrex::cellconslin_interp(tbx, *finep, fine_comp, ncomp, *ccfab, *crsep, crse_comp,
                                      voff, ratio);
        });
    } else {
        const Box& fslope_bx = amrex::refine(cslope_bx,ratio);
        AsyncFab as_fafab(fslope_bx, ncomp);
        FArrayBox* fafab = as_fafab.fabPtr();

        AMREX_LAUNCH_HOST_DEVICE_LAMBDA (cslope_bx, tbx,
        {
            amrex::cellconslin_slopes_mclim(tbx, *ccfab, *crsep, crse_comp, ncomp, bcrp);
        });

        AMREX_LAUNCH_HOST_DEVICE_LAMBDA (fslope_bx, tbx,
        {
            amrex::cellconslin_fine_alpha(tbx, *fafab, *ccfab, ncomp, voff, ratio);
        });

        AMREX_LAUNCH_HOST_DEVICE_LAMBDA (cslope_bx, tbx,
        {
            amrex::cellconslin_slopes_mmlim(tbx, *ccfab, *fafab, ncomp, ratio);
        });

        AMREX_LAUNCH_HOST_DEVICE_LAMBDA (fine_region, tbx,
        {
            amrex::cellconslin_interp(tbx, *finep, fine_comp, ncomp, *ccfab, *crsep, crse_comp,
                                      voff, ratio);
        });
    }
}

CellQuadratic::CellQuadratic (bool limit)
{
    do_limited_slope = limit;
}

CellQuadratic::~CellQuadratic () {}

Box
CellQuadratic::CoarseBox (const Box&     fine,
                          const IntVect& ratio)
{
    Box crse = amrex::coarsen(fine,ratio);
    crse.grow(1);
    return crse;
}

Box
CellQuadratic::CoarseBox (const Box& fine,
                          int        ratio)
{
    Box crse = amrex::coarsen(fine,ratio);
    crse.grow(1);
    return crse;
}

void
CellQuadratic::interp (const FArrayBox& crse,
                       int              crse_comp,
                       FArrayBox&       fine,
                       int              fine_comp,
                       int              ncomp,
                       const Box&       fine_region,
                       const IntVect&   ratio,
                       const Geometry&  crse_geom,
                       const Geometry&  fine_geom,
                       Vector<BCRec>&    bcr,
                       int              actual_comp,
                       int              actual_state)
{
    BL_PROFILE("CellQuadratic::interp()");
    BL_ASSERT(bcr.size() >= ncomp);
    //
    // Make box which is intersection of fine_region and domain of fine.
    //
    Box target_fine_region = fine_region & fine.box();

    Box crse_bx(amrex::coarsen(target_fine_region,ratio));
    Box fslope_bx(amrex::refine(crse_bx,ratio));
    Box cslope_bx(crse_bx);
    cslope_bx.grow(1);
    BL_ASSERT(crse.box().contains(cslope_bx));
    //
    // Alloc temp space for coarse grid slopes: here we use 5
    // instead of AMREX_SPACEDIM because of the x^2, y^2 and xy terms
    //
    long t_long = cslope_bx.numPts();
    BL_ASSERT(t_long < INT_MAX);
    int c_len = int(t_long);

    Vector<Real> cslope(5*c_len);

    int loslp = cslope_bx.index(crse_bx.smallEnd());
    int hislp = cslope_bx.index(crse_bx.bigEnd());

    t_long = cslope_bx.numPts();
    BL_ASSERT(t_long < INT_MAX);
    int cslope_vol = int(t_long);
    int clo        = 1 - loslp;
    int chi        = clo + cslope_vol - 1;
    c_len          = hislp - loslp + 1;
    //
    // Alloc temp space for one strip of fine grid slopes: here we use 5
    // instead of AMREX_SPACEDIM because of the x^2, y^2 and xy terms.
    //
    int dir;
    int f_len = fslope_bx.longside(dir);

    Vector<Real> strip((5+2)*f_len);

    Real* fstrip = strip.dataPtr();
    Real* foff   = fstrip + f_len;
    Real* fslope = foff + f_len;
    //
    // Get coarse and fine edge-centered volume coordinates.
    //
    Vector<Real> fvc[AMREX_SPACEDIM];
    Vector<Real> cvc[AMREX_SPACEDIM];
    for (dir = 0; dir < AMREX_SPACEDIM; dir++)
    {
        fine_geom.GetEdgeVolCoord(fvc[dir],target_fine_region,dir);
        crse_geom.GetEdgeVolCoord(cvc[dir],crse_bx,dir);
    }
    //
    // Alloc tmp space for slope calc and to allow for vectorization.
    //
    Real* fdat        = fine.dataPtr(fine_comp);
    const Real* cdat  = crse.dataPtr(crse_comp);
    const int* flo    = fine.loVect();
    const int* fhi    = fine.hiVect();
    const int* fblo   = target_fine_region.loVect();
    const int* fbhi   = target_fine_region.hiVect();
    const int* cblo   = crse_bx.loVect();
    const int* cbhi   = crse_bx.hiVect();
    const int* fslo   = fslope_bx.loVect();
    const int* fshi   = fslope_bx.hiVect();
    int slope_flag    = (do_limited_slope ? 1 : 0);
    Vector<int> bc     = GetBCArray(bcr);
    const int* ratioV = ratio.getVect();

#if (AMREX_SPACEDIM > 1)

    amrex_cqinterp (fdat,AMREX_ARLIM(flo),AMREX_ARLIM(fhi),
                   AMREX_ARLIM(fblo), AMREX_ARLIM(fbhi),
                   &ncomp,AMREX_D_DECL(&ratioV[0],&ratioV[1],&ratioV[2]),
                   cdat,&clo,&chi,
                   AMREX_ARLIM(cblo), AMREX_ARLIM(cbhi),
                   fslo,fshi,
                   cslope.dataPtr(),&c_len,fslope,fstrip,&f_len,foff,
                   bc.dataPtr(), &slope_flag,
                   AMREX_D_DECL(fvc[0].dataPtr(),fvc[1].dataPtr(),fvc[2].dataPtr()),
                   AMREX_D_DECL(cvc[0].dataPtr(),cvc[1].dataPtr(),cvc[2].dataPtr()),
                   &actual_comp,&actual_state);

#endif /*(AMREX_SPACEDIM > 1)*/
}

PCInterp::~PCInterp () {}

Box
PCInterp::CoarseBox (const Box& fine,
                     int        ratio)
{
    return amrex::coarsen(fine,ratio);
}

Box
PCInterp::CoarseBox (const Box&     fine,
                     const IntVect& ratio)
{
    return amrex::coarsen(fine,ratio);
}

void
PCInterp::interp (const FArrayBox& crse,
                  int              crse_comp,
                  FArrayBox&       fine,
                  int              fine_comp,
                  int              ncomp,
                  const Box&       fine_region,
                  const IntVect&   ratio,
                  const Geometry& /*crse_geom*/,
                  const Geometry& /*fine_geom*/,
                  Vector<BCRec>&   /*bcr*/,
                  int               /*actual_comp*/,
                  int               /*actual_state*/)
{
    BL_PROFILE("PCInterp::interp()");

    FArrayBox const* crsep = &crse;
    FArrayBox* finep = &fine;

    Gpu::LaunchSafeGuard lg(Gpu::isGpuPtr(crsep) && Gpu::isGpuPtr(finep));

    AMREX_LAUNCH_HOST_DEVICE_LAMBDA (fine_region, tbx,
    {
        amrex::pcinterp_interp(tbx,*finep,fine_comp,ncomp,*crsep,crse_comp,ratio);
    });
}

CellConservativeProtected::CellConservativeProtected () {}

CellConservativeProtected::~CellConservativeProtected () {}

Box
CellConservativeProtected::CoarseBox (const Box&     fine,
                                      const IntVect& ratio)
{
    Box crse = amrex::coarsen(fine,ratio);
    crse.grow(1);
    return crse;
}

Box
CellConservativeProtected::CoarseBox (const Box& fine,
                                      int        ratio)
{
    Box crse(amrex::coarsen(fine,ratio));
    crse.grow(1);
    return crse;
}

void
CellConservativeProtected::interp (const FArrayBox& crse,
                                   int              crse_comp,
                                   FArrayBox&       fine,
                                   int              fine_comp,
                                   int              ncomp,
                                   const Box&       fine_region,
                                   const IntVect&   ratio,
                                   const Geometry&  crse_geom,
                                   const Geometry&  fine_geom,
                                   Vector<BCRec>&    bcr,
                                   int              actual_comp,
                                   int              actual_state)
{
    BL_PROFILE("CellConservativeProtected::interp()");
    BL_ASSERT(bcr.size() >= ncomp);

    AMREX_ASSERT(fine.box().contains(fine_region));

    FArrayBox const* crsep = &crse;
    FArrayBox* finep = &fine;

    Gpu::LaunchSafeGuard lg(Gpu::isGpuPtr(crsep) && Gpu::isGpuPtr(finep));

    const Box& crse_region = CoarseBox(fine_region,ratio);
    const Box& cslope_bx = amrex::grow(crse_region,-1);

    AsyncArray<BCRec> async_bcr(bcr.data(), ncomp);
    BCRec* bcrp = async_bcr.data();

    // component of ccfab : slopes for first compoent for x-direction
    //                      slopes for second component for x-direction
    //                      ...
    //                      slopes for last component for x-direction
    //                      slopes for y-direction
    //                      slopes for z-drction
    // then followed by
    //                      factors (one for all components) for x, y and z-direction
    const int ntmp = (ncomp+1)*AMREX_SPACEDIM;
    AsyncFab as_ccfab(cslope_bx, ntmp);
    FArrayBox* ccfab = as_ccfab.fabPtr();

    const Vector<Real>& vec_voff = amrex::ccinterp_compute_voff(cslope_bx, ratio, crse_geom, fine_geom);

    AsyncArray<Real> async_voff(vec_voff.data(), vec_voff.size());
    Real const* voff = async_voff.data();

    AMREX_LAUNCH_HOST_DEVICE_LAMBDA (cslope_bx, tbx,
    {
        amrex::cellconslin_slopes_linlim(tbx, *ccfab, *crsep, crse_comp, ncomp, bcrp);
    });

    AMREX_LAUNCH_HOST_DEVICE_LAMBDA (fine_region, tbx,
    {
        amrex::cellconslin_interp(tbx, *finep, fine_comp, ncomp, *ccfab, *crsep, crse_comp,
                                  voff, ratio);
    });
}

void
CellConservativeProtected::protect (const FArrayBox& crse,
                                    int              crse_comp,
                                    FArrayBox&       fine,
                                    int              fine_comp,
                                    FArrayBox&       fine_state,
                                    int              state_comp,
                                    int              ncomp,
                                    const Box&       fine_region,
                                    const IntVect&   ratio,
                                    const Geometry&  crse_geom,
                                    const Geometry&  fine_geom,
                                    Vector<BCRec>& bcr)
{
    BL_PROFILE("CellConservativeProtected::protect()");
    BL_ASSERT(bcr.size() >= ncomp);

    //
    // Make box which is intersection of fine_region and domain of fine.
    //
    Box target_fine_region = fine_region & fine.box();

    //
    // crse_bx is coarsening of target_fine_region, grown by 1.
    //
    Box crse_bx = CoarseBox(target_fine_region,ratio);

    //
    // cs_bx is coarsening of target_fine_region.
    //
    Box cs_bx(crse_bx);
    cs_bx.grow(-1);

    //
    // Get coarse and fine edge-centered volume coordinates.
    //
    int dir;
    Vector<Real> fvc[AMREX_SPACEDIM];
    Vector<Real> cvc[AMREX_SPACEDIM];
    for (dir = 0; dir < AMREX_SPACEDIM; dir++)
    {
        fine_geom.GetEdgeVolCoord(fvc[dir],target_fine_region,dir);
        crse_geom.GetEdgeVolCoord(cvc[dir],crse_bx,dir);
    }

#if (AMREX_SPACEDIM == 2)
    const int* cvcblo = crse_bx.loVect();
    const int* fvcblo = target_fine_region.loVect();

    int cvcbhi[AMREX_SPACEDIM];
    int fvcbhi[AMREX_SPACEDIM];

    for (dir=0; dir<AMREX_SPACEDIM; dir++)
    {
        cvcbhi[dir] = cvcblo[dir] + cvc[dir].size() - 1;
        fvcbhi[dir] = fvcblo[dir] + fvc[dir].size() - 1;
    }
#endif

    Real* fdat       = fine.dataPtr(fine_comp);
    Real* state_dat  = fine_state.dataPtr(state_comp);
    const Real* cdat = crse.dataPtr(crse_comp);

    const int* flo    = fine.loVect();
    const int* fhi    = fine.hiVect();
    const int* slo    = fine_state.loVect();
    const int* shi    = fine_state.hiVect();
    const int* clo    = crse.loVect();
    const int* chi    = crse.hiVect();
    const int* fblo   = target_fine_region.loVect();
    const int* fbhi   = target_fine_region.hiVect();
    const int* csbhi  = cs_bx.hiVect();
    const int* csblo  = cs_bx.loVect();

    Vector<int> bc     = GetBCArray(bcr);
    const int* ratioV = ratio.getVect();

#if (AMREX_SPACEDIM > 1)

    amrex_protect_interp (fdat,AMREX_ARLIM(flo),AMREX_ARLIM(fhi),
                         fblo, fbhi,
                         cdat,AMREX_ARLIM(clo),AMREX_ARLIM(chi),
                         csblo, csbhi,
#if (AMREX_SPACEDIM == 2)
                         fvc[0].dataPtr(),fvc[1].dataPtr(),
                         AMREX_ARLIM(fvcblo), AMREX_ARLIM(fvcbhi),
                         cvc[0].dataPtr(),cvc[1].dataPtr(),
                         AMREX_ARLIM(cvcblo), AMREX_ARLIM(cvcbhi),
#endif
                         state_dat, AMREX_ARLIM(slo), AMREX_ARLIM(shi),
                         &ncomp,AMREX_D_DECL(&ratioV[0],&ratioV[1],&ratioV[2]),
                         bc.dataPtr());

#endif /*(AMREX_SPACEDIM > 1)*/

}

CellConservativeQuartic::~CellConservativeQuartic () {}

Box
CellConservativeQuartic::CoarseBox (const Box& fine,
				    int        ratio)
{
    Box crse(amrex::coarsen(fine,ratio));
    crse.grow(2);
    return crse;
}

Box
CellConservativeQuartic::CoarseBox (const Box&     fine,
				    const IntVect& ratio)
{
    Box crse = amrex::coarsen(fine,ratio);
    crse.grow(2);
    return crse;
}

void
CellConservativeQuartic::interp (const FArrayBox&  crse,
				 int               crse_comp,
				 FArrayBox&        fine,
				 int               fine_comp,
				 int               ncomp,
				 const Box&        fine_region,
				 const IntVect&    ratio,
				 const Geometry&   /* crse_geom */,
				 const Geometry&   /* fine_geom */,
				 Vector<BCRec>&     bcr,
				 int               actual_comp,
				 int               actual_state)
{
    BL_PROFILE("CellConservativeQuartic::interp()");
    BL_ASSERT(bcr.size() >= ncomp);
    BL_ASSERT(ratio[0]==2);
#if (AMREX_SPACEDIM >= 2)
    BL_ASSERT(ratio[0] == ratio[1]);
#endif
#if (AMREX_SPACEDIM == 3)
    BL_ASSERT(ratio[1] == ratio[2]);
#endif

    //
    // Make box which is intersection of fine_region and domain of fine.
    //
    Box target_fine_region = fine_region & fine.box();
    //
    // crse_bx is coarsening of target_fine_region, grown by 2.
    //
    Box crse_bx = CoarseBox(target_fine_region,ratio);

    Box crse_bx2(crse_bx);
    crse_bx2.grow(-2);
    Box fine_bx2 = amrex::refine(crse_bx2,ratio);

    Real* fdat       = fine.dataPtr(fine_comp);
    const Real* cdat = crse.dataPtr(crse_comp);

    const int* flo    = fine.loVect();
    const int* fhi    = fine.hiVect();
    const int* clo    = crse.loVect();
    const int* chi    = crse.hiVect();
    const int* fblo   = target_fine_region.loVect();
    const int* fbhi   = target_fine_region.hiVect();
    const int* cblo   = crse_bx.loVect();
    const int* cbhi   = crse_bx.hiVect();
    const int* cb2lo  = crse_bx2.loVect();
    const int* cb2hi  = crse_bx2.hiVect();
    const int* fb2lo  = fine_bx2.loVect();
    const int* fb2hi  = fine_bx2.hiVect();

    Vector<int> bc     = GetBCArray(bcr);
    const int* ratioV = ratio.getVect();

    int ltmp = fb2hi[0]-fb2lo[0]+1;
    Vector<Real> ftmp(ltmp);

#if (AMREX_SPACEDIM >= 2)
    ltmp = (cbhi[0]-cblo[0]+1)*ratio[1];
    Vector<Real> ctmp(ltmp);
#endif

#if (AMREX_SPACEDIM == 3)
    ltmp = (cbhi[0]-cblo[0]+1)*(cbhi[1]-cblo[1]+1)*ratio[2];
    Vector<Real> ctmp2(ltmp);
#endif

    amrex_quartinterp (fdat,AMREX_ARLIM(flo),AMREX_ARLIM(fhi),
		      fblo, fbhi, fb2lo, fb2hi,
		      cdat,AMREX_ARLIM(clo),AMREX_ARLIM(chi),
		      cblo, cbhi, cb2lo, cb2hi,
		      &ncomp,
		      AMREX_D_DECL(&ratioV[0],&ratioV[1],&ratioV[2]),
		      AMREX_D_DECL(ftmp.dataPtr(), ctmp.dataPtr(), ctmp2.dataPtr()),
		      bc.dataPtr(),&actual_comp,&actual_state);
}

//Cell GP interp SR Dissertation Work 
CellGaussianProcess::CellGaussianProcess (bool mult_sample)
{
    do_multi_sampled = mult_sample;
}

CellGaussianProcess::~CellGaussianProcess () {}

Box
CellGaussianProcess::CoarseBox (const Box&     fine,
                          const IntVect& ratio)
{
    Box crse = amrex::coarsen(fine,ratio);
    crse.grow(1);
    return crse;
}

Box
CellGaussianProcess::CoarseBox (const Box& fine,
                          int        ratio)
{
    Box crse = amrex::coarsen(fine,ratio);
    crse.grow(1);
    return crse;
}
 
//Builds the Covariance matrix K if uninitialized --> if(!init) GetK, weights etc.
//Four K totals to make the gammas.  
void
CellGaussianProcess::GetK(amrex::Real *K, amrex::Real *Ktot)
{

    int pnt[5][2];
    amrex::Real arg;  
    pnt[0][0] = 0 , pnt[0][1] = -1; 
    pnt[1][0] = -1, pnt[1][1] = 0; 
    pnt[2][0] = 0 , pnt[2][1] = 0; 
    pnt[3][0] = 1 , pnt[3][1] = 0; 
    pnt[4][0] = 0 , pnt[4][1] = 1; 

    for(int i = 0; i < 5; ++i) K[i + 5*i] = 1.e0; 
//Small K
    for(int i = 1; i < 5; ++i)
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[i][0] - pnt[j][0])*dx[0],2)
                            + pow(double(pnt[i][1] - pnt[j][1])*dx[1],2);
            arg /= pow(l,2); 
            K[j + 5*i] = exp(-0.5*arg); 
        }

// "Super K"s 

//TODO double check index maths.     
    for(int k = 0; k < 4; ++k)
        for(int i = 0; i < 10; ++i) Ktot[i + 10*i + 100*k] = 1.e0; 
    int spnt[10][2]; 
// i-1/4,j-1/4 
    spnt[0][0] =  0, spnt[0][1] = -2; 
    spnt[1][0] = -1, spnt[1][1] = -1; 
    spnt[2][0] =  0, spnt[2][1] = -1; 
    spnt[3][0] =  1, spnt[3][1] = -1; 
    spnt[4][0] = -2, spnt[4][1] =  0; 
    spnt[5][0] = -1, spnt[5][1] =  0; 
    spnt[6][0] =  0, spnt[6][1] =  0; 
    spnt[7][0] =  1, spnt[7][1] =  0; 
    spnt[8][0] = -1, spnt[8][1] =  1;
    spnt[9][0] =  0, spnt[9][1] =  1; 

    for(int i = 1; i < 10; ++i)
        for(int j = i; j <10; ++j){
            arg = pow(double(spnt[i][0] - spnt[j][0])*dx[0],2)
                            + pow(double(spnt[i][1] - spnt[j][1])*dx[1],2);
            arg /= pow(l,2); 
            Ktot[j + 10*i] = exp(-0.5e0*arg); 
        }
// i+1/4,j-1/4 
    spnt[0][0] =  0, spnt[0][1] = -2; 
    spnt[1][0] = -1, spnt[1][1] = -1; 
    spnt[2][0] =  0, spnt[2][1] = -1; 
    spnt[3][0] =  1, spnt[3][1] = -1; 
    spnt[4][0] = -1, spnt[4][1] =  0; 
    spnt[5][0] =  0, spnt[5][1] =  0; 
    spnt[6][0] =  1, spnt[6][1] =  0; 
    spnt[7][0] =  2, spnt[7][1] =  0; 
    spnt[8][0] =  0, spnt[8][1] =  1;
    spnt[9][0] =  1, spnt[9][1] =  1; 

    for(int i = 1; i < 10; ++i)
        for(int j = i; j <10; ++j){
            arg = pow(double(spnt[i][0] - spnt[j][0])*dx[0],2)
                            + pow(double(spnt[i][1] - spnt[j][1])*dx[1],2);
            arg /= pow(l,2); 
            Ktot[j + 10*i + 100] = exp(-0.5e0*arg); 
        }

// i-1/4,j+1/4 
    spnt[0][0] = -1, spnt[0][1] = -1; 
    spnt[1][0] =  0, spnt[1][1] = -1; 
    spnt[2][0] = -2, spnt[2][1] =  0; 
    spnt[3][0] = -1, spnt[3][1] =  0; 
    spnt[4][0] =  0, spnt[4][1] =  0; 
    spnt[5][0] =  1, spnt[5][1] =  0; 
    spnt[6][0] = -1, spnt[6][1] =  1; 
    spnt[7][0] =  0, spnt[7][1] =  1; 
    spnt[8][0] =  1, spnt[8][1] =  1;
    spnt[9][0] =  0, spnt[9][1] =  2; 

    for(int i = 1; i < 10; ++i)
        for(int j = i; j <10; ++j){
            arg = pow(double(spnt[i][0] - spnt[j][0])*dx[0],2)
                            + pow(double(spnt[i][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            Ktot[j + 10*i + 200] = exp(-0.5e0*arg); 
        }
// i+1/4,j+1/4 
    spnt[0][0] =  0, spnt[0][1] = -1; 
    spnt[1][0] =  1, spnt[1][1] = -1; 
    spnt[2][0] = -1, spnt[2][1] =  0; 
    spnt[3][0] =  0, spnt[3][1] =  0; 
    spnt[4][0] =  1, spnt[4][1] =  0; 
    spnt[5][0] =  2, spnt[5][1] =  0; 
    spnt[6][0] = -1, spnt[6][1] =  1; 
    spnt[7][0] =  0, spnt[7][1] =  1; 
    spnt[8][0] =  1, spnt[8][1] =  1;
    spnt[9][0] =  0, spnt[9][1] =  2; 

    for(int i = 1; i < 10; ++i)
        for(int j = i; j <10; ++j){
            arg = pow(double(spnt[i][0] - spnt[j][0])*dx[0],2)
                            + pow(double(spnt[i][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            Ktot[j + 10*i + 300] = exp(-0.5e0*arg); 
        }
}

//Use a Cholesky Decomposition to solve for k*K^-1 
//Inputs: K, outputs w = k*K^-1. 
//Here we have 12 vectors per quadrant. K is shift invariant, so only k* matters. 
void 
CellGaussianProcess::GetKs(amrex::Real const *K, amrex::Real const *dx)
{

    //Locations of new points relative to i,j 
    amrex::Real pnt[16][2]; 
    pnt[0][0] = -.375,  pnt[0][1] = -.375; 
    pnt[1][0] = -.125,  pnt[1][1] = -.375; 
    pnt[2][0] = 0.125,  pnt[2][1] = -.375; 
    pnt[3][0] = 0.375,  pnt[3][1] = -.375; 
    pnt[4][0] = -.375,  pnt[4][1] = -.125; 
    pnt[5][0] = -.125,  pnt[5][1] = -.125; 
    pnt[6][0] = 0.125,  pnt[6][1] = -.125; 
    pnt[7][0] = 0.375,  pnt[7][1] = -.125; 
    pnt[8][0] = -.375,  pnt[8][1] = 0.125; 
    pnt[9][0] = -.125,  pnt[9][1] = 0.125; 
    pnt[10][0] = 0.125, pnt[10][1] = 0.125; 
    pnt[11][0] = 0.375, pnt[11][1] = 0.125; 
    pnt[12][0] = -.375, pnt[12][1] = 0.375; 
    pnt[13][0] = -.125, pnt[13][1] = 0.375; 
    pnt[14][0] = 0.125, pnt[14][1] = 0.375; 
    pnt[15][0] = 0.375, pnt[15][1] = 0.375; 

    int spnt[5][2]; 
    spnt[0][0] = 0 , spnt[0][1] = -1; 
    spnt[1][0] = -1, spnt[1][1] = 0; 
    spnt[2][0] = 0 , spnt[2][1] = 0; 
    spnt[3][0] = 1 , spnt[3][1] = 0; 
    spnt[4][0] = 0 , spnt[4][1] = 1; 

    amrex::Real kstar[4][5];
//================ Quadrant one i-1/4, j-1/4, three stencils. ======================

    //Stencil one centered at i, j-1 
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[i][0] - spnt[j][0])*dx[0],2)
                            + pow(double(pnt[i][1] - (spnt[j][1] -1))*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[i][0]); 
    }
     
    //Stencil two centered at i-1, j 
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[i][0] - (spnt[j][0] - 1))*dx[0],2)
                            + pow(double(pnt[i][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[i][1]); 
    }
     
    //Stencil three is standard i,j 
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[i][0] - spnt[j][0])*dx[0],2)
                            + pow(double(pnt[i][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[i][2]); 
    }
     
//================= Quadrant two i+1/4, j-1/4, three stencils. ===================

    //Stencil one centered at i, j-1 
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        int id = i + 4; 
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[id][0] - spnt[j][0])*dx[0],2)
                            + pow(double(pnt[id][1] - (spnt[j][1] -1))*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[id][0]); 
    }
     
    //Stencil two centered at i, j 
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        int id = i + 4; 
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[id][0] - spnt[j][0])*dx[0],2)
                            + pow(double(pnt[id][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[id][1]); 
    }
     
    //Stencil three centered i+1,j 
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        int id = i + 4; 
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[id][0] - (spnt[j][0] + 1))*dx[0],2)
                            + pow(double(pnt[id][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[id][2]); 
    }

//============== Quadrant three i-1/4, j+1/4, three stencils. ================

    //Stencil one centered at i-1, j
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        int id = i + 8; 
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[id][0] - (spnt[j][0]-1))*dx[0],2)
                            + pow(double(pnt[id][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[id][0]); 
    }
     
    //Stencil two centered at i, j 
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        int id = i + 8; 
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[id][0] - spnt[j][0])*dx[0],2)
                            + pow(double(pnt[id][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[id][1]); 
    }
     
    //Stencil three centered i+1,j 
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        int id = i + 8; 
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[id][0] - (spnt[j][0] + 1))*dx[0],2)
                            + pow(double(pnt[id][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[id][2]); 
    }

//============== Quadrant four i+1/4, j+1/4, three stencils. ==============

    //Stencil one centered at i, j
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        int id = i + 12; 
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[id][0] - spnt[j][0])*dx[0],2)
                            + pow(double(pnt[id][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[id][0]); 
    }
     
    //Stencil two centered at i+1, j 
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        int id = i + 12; 
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[id][0] - (spnt[j][0] + 1))*dx[0],2)
                            + pow(double(pnt[id][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[id][1]); 
    }
     
    //Stencil three centered i,j+1
    //Build covariance vector between interpolant points and stencil 
     for(int i = 0; i < 4; ++i){
        int id = i + 12; 
        for(int j = i; j < 5; ++j){
            arg = pow(double(pnt[id][0] - spnt[j][0])*dx[0],2)
                            + pow(double(pnt[id][1] - (spnt[j][1]+1))*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[id][2]); 
    }
}

//Perfroms Cholesky Decomposition on covariance matrix K
void
CellGaussianProcess::CholeskyDecomp(amrex::Real *K, const int n)
{
     for(int i = 0; i < n; ++i){
        for( int j = i; j < n; ++j){
            K[i + n*i] -= K[j + n*i]*K[j + n*i]; 
        }
        K[i + n*i] = sqrt(K[i + n*i]); 
        for(int j = i+1; j < n; ++j){
            for(int k = 0; k < j; ++j){
                K[j + n*i] -= K[j + n*k]*K[i + n*k]; 
            }
        K[j + n*i] /= K[j + n*j]; 
        }
    }
}

//Performs Cholesky Backsubstitution 
void 
CellGaussianProcess::cholesky(amrex::Real const *kstar, amrex::Real const *K, 
                              amrex::Real *ks, const int n)
{
    /* Forward sub Ly = b */ 
    for(int i = 0; i < n; ++i){
        ks[i] = 0.0e0; 
        for(int j = 0; j < i; ++j)
            ks[i] = ks[i] + kstar[j]*K[j + 5*i]; 
        ks[i] /= K[i + n*i]; 
    }
    /* Back sub Ux = y */ 
    for(int i = n-1; i >= 0; --i){
        for(int j = i; j < n; ++j)
            ks[i] -= K[j + n*i]*ks[j]; 
        ks[i] /= K[i + n*i]; 
    }   
}

// Here we are using the 4 Ktotols to get the total weights for each quadrant. 
// In this case, we will have 4 new points per quadrant 
// Therefore, we will need 16 k*^T Ktot^(-1) 
void 
CellGaussianProcess::GetKtotks(const amrex::Real *Ktot, const amrex::Real ks[16][])
{
    //Locations of new points relative to i,j 
    amrex::Real pnt[16][2]; 
    pnt[0][0] = -.375,  pnt[0][1] = -.375; 
    pnt[1][0] = -.125,  pnt[1][1] = -.375; 
    pnt[2][0] = 0.125,  pnt[2][1] = -.375; 
    pnt[3][0] = 0.375,  pnt[3][1] = -.375; 
    pnt[4][0] = -.375,  pnt[4][1] = -.125; 
    pnt[5][0] = -.125,  pnt[5][1] = -.125; 
    pnt[6][0] = 0.125,  pnt[6][1] = -.125; 
    pnt[7][0] = 0.375,  pnt[7][1] = -.125; 
    pnt[8][0] = -.375,  pnt[8][1] = 0.125; 
    pnt[9][0] = -.125,  pnt[9][1] = 0.125; 
    pnt[10][0] = 0.125, pnt[10][1] = 0.125; 
    pnt[11][0] = 0.375, pnt[11][1] = 0.125; 
    pnt[12][0] = -.375, pnt[12][1] = 0.375; 
    pnt[13][0] = -.125, pnt[13][1] = 0.375; 
    pnt[14][0] = 0.125, pnt[14][1] = 0.375; 
    pnt[15][0] = 0.375, pnt[15][1] = 0.375; 

    amrex::Real *K; 

    //Super K positions 
    int spnt[10][2]; 
// i-1/4,j-1/4 
    spnt[0][0] =  0, spnt[0][1] = -2; 
    spnt[1][0] = -1, spnt[1][1] = -1; 
    spnt[2][0] =  0, spnt[2][1] = -1; 
    spnt[3][0] =  1, spnt[3][1] = -1; 
    spnt[4][0] = -2, spnt[4][1] =  0; 
    spnt[5][0] = -1, spnt[5][1] =  0; 
    spnt[6][0] =  0, spnt[6][1] =  0; 
    spnt[7][0] =  1, spnt[7][1] =  0; 
    spnt[8][0] = -1, spnt[8][1] =  1;
    spnt[9][0] =  0, spnt[9][1] =  1;
    
/*    for(int i = 0; i < 10; ++i)
        for(int j = 0; j < 10; ++j)
            K[j + 10*i] = Ktot[j + 10*i]; 
*/ 
   K = &Ktot; 
   CholeskyDecomp(K, 10); 
    
    for(int i = 0; i < 4; ++i){
       for(int j = i; j < 10; ++j){
           arg = pow(double(pnt[i][0] - spnt[j][0])*dx[0],2)
               + pow(double(pnt[i][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[i], 10); 
    }
  
    // i+1/4,j-1/4 
    spnt[0][0] =  0, spnt[0][1] = -2; 
    spnt[1][0] = -1, spnt[1][1] = -1; 
    spnt[2][0] =  0, spnt[2][1] = -1; 
    spnt[3][0] =  1, spnt[3][1] = -1; 
    spnt[4][0] = -1, spnt[4][1] =  0; 
    spnt[5][0] =  0, spnt[5][1] =  0; 
    spnt[6][0] =  1, spnt[6][1] =  0; 
    spnt[7][0] =  2, spnt[7][1] =  0; 
    spnt[8][0] =  0, spnt[8][1] =  1;
    spnt[9][0] =  1, spnt[9][1] =  1; 

/*    for(int i = 0; i < 10; ++i)
        for(int j = 0; j < 10; ++j)
            K[j + 10*i] = Ktot[j + 10*i + 100]; 
*/ 
   K = &Ktot + 100; 
   CholeskyDecomp(K, 10); 
    
    for(int i = 4; i < 8; ++i){
       for(int j = i; j < 10; ++j){
           arg = pow(double(pnt[i][0] - spnt[j][0])*dx[0],2)
               + pow(double(pnt[i][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[i], 10); 
    }

// i-1/4,j+1/4 
    spnt[0][0] = -1, spnt[0][1] = -1; 
    spnt[1][0] =  0, spnt[1][1] = -1; 
    spnt[2][0] = -2, spnt[2][1] =  0; 
    spnt[3][0] = -1, spnt[3][1] =  0; 
    spnt[4][0] =  0, spnt[4][1] =  0; 
    spnt[5][0] =  1, spnt[5][1] =  0; 
    spnt[6][0] = -1, spnt[6][1] =  1; 
    spnt[7][0] =  0, spnt[7][1] =  1; 
    spnt[8][0] =  1, spnt[8][1] =  1;
    spnt[9][0] =  0, spnt[9][1] =  2; 
/*    for(int i = 0; i < 10; ++i)
        for(int j = 0; j < 10; ++j)
            K[j + 10*i] = Ktot[j + 10*i + 200]; 
*/ 
   K = &Ktot + 200; 
   CholeskyDecomp(K, 10); 
    
    for(int i = 8; i
 < 12; ++i){
       for(int j = i; j < 10; ++j){
           arg = pow(double(pnt[i][0] - spnt[j][0])*dx[0],2)
               + pow(double(pnt[i][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[i], 10); 
    }

// i+1/4,j+1/4 
    spnt[0][0] =  0, spnt[0][1] = -1; 
    spnt[1][0] =  1, spnt[1][1] = -1; 
    spnt[2][0] = -1, spnt[2][1] =  0; 
    spnt[3][0] =  0, spnt[3][1] =  0; 
    spnt[4][0] =  1, spnt[4][1] =  0; 
    spnt[5][0] =  2, spnt[5][1] =  0; 
    spnt[6][0] = -1, spnt[6][1] =  1; 
    spnt[7][0] =  0, spnt[7][1] =  1; 
    spnt[8][0] =  1, spnt[8][1] =  1;
    spnt[9][0] =  0, spnt[9][1] =  2; 

/*    for(int i = 0; i < 10; ++i)
        for(int j = 0; j < 10; ++j)
            K[j + 10*i] = Ktot[j + 10*i + 300]; */ 
   K = &Ktot + 300; 
   CholeskyDecomp(K, 10); 
    
    for(int i = 12; i < 16; ++i){
       for(int j = i; j < 10; ++j){
           arg = pow(double(pnt[i][0] - spnt[j][0])*dx[0],2)
               + pow(double(pnt[i][1] - spnt[j][1])*dx[1],2); 
            arg /= pow(l,2); 
            kstar[i][j] = exp(-arg); 
        }
        cholesky(kstar[i], K, ks[i], 10); 
    }

}


//This will need to be called from a bigger function for 
//each point in each quadrant. Essentially each point will have its
//own set of gammas. 
void
CellGaussianProcess::GetGamma(amrex::Real const ks[3][5],
                              amrex::Real const Kt[10], 
                              amrex::Real &gam[3])
{
// We will try the normal equations for this portion. So we need to construct 
// each partitions' over-determined system.
 
    amrex::Real A[9] = {}; 
    amrex::Real RHS[3] = {}; 

//Fill A from data

    for(int i = 0; i < 5; ++i){
        A[0] += ks[0][i]*ks[0][i]; 
        A[4] += ks[1][i]*ks[1][i]; 
        A[8] += ks[2][i]*ks[2][i]; 
    }

    A[1] = ks[0][2]*ks[1][1] + ks[0][3]*ks[1][2]; 
    A[3] = A[1]; 
    A[2] = ks[0][1]*ks[2][0] + ks[0][3]*ks[2][1]; 
    A[6] = A[2]; 
    A[5] = ks[2][1]*ks[1][2] + ks[1][3]*ks[2][2]; 
    A[7] = A[5]; 
    CholeskyDecomp(A, 3); 

//Fill RHS 
    RHS[0] = ks[0][0]*Kt[0] + ks[0][1]*Kt[2] + ks[0][2]*Kt[3]
           + ks[0][3]*Kt[4] + ks[0][4]*Kt[7]; 

    RHS[1] = ks[1][0]*Kt[1] + ks[1][1]*Kt[3] + ks[1][2]*Kt[4]
           + ks[1][3]*Kt[5] + ks[1][4]*Kt[8]; 

    RHS[2] = ks[2][0]*Kt[2] + ks[2][1]*Kt[4] + ks[2][2]*Kt[5]
           + ks[2][3]*Kt[6] + ks[2][4]*Kt[9]; 

    cholesky(RHS,A,gam,3); 
}

//Use Shifted QR with deflation to get eigen pairs. 
void 
CellGaussianProcess::GetEigenPairs(const amrex::Real *K)
{
    std::vector<amrex::Real, 25> p; 
    hessen(K, p);
    for(int j = 5; j > 1; j--){
        amrex::Real er = 1.e0; 
        while(er> 1.e-10){
            amrex::Real mu = K[j + 5*j]; 
#pragma unroll 
            for(int i = 0; i < 5; i++)
                P[i + 5*i] -= mu;   
            qr_decomp(B, Q); 
            qr_appl(B, Q, 5); 
            if(j < 5){
                qr_appl(P, q, j);                 
            }
            for(int i = 0; i < j; i++)
#pragma unroll
                for(int k = 0; k < j; k++)
                    V_iter[i][k] = Q[i][k];
            
            q_appl(V,V_iter,5); 
#pragma unroll
            for(int i = 0; i < 5; i++)
                B[i + 5*i] += mu; 

            er = fabs(B[j + 5*(j-1)]); 
        }
    }
//Since K is symmetric the eigenvectors are converged. 
}


void
CellGaussianProcess::interp (const FArrayBox& crse,
                       int              crse_comp,
                       FArrayBox&       fine,
                       int              fine_comp,
                       int              ncomp,
                       const Box&       fine_region,
                       const IntVect&   ratio,
                       const Geometry&  crse_geom,
                       const Geometry&  fine_geom,
                       Vector<BCRec>&    bcr,
                       int              actual_comp,
                       int              actual_state)
{
    BL_PROFILE("CellGaussianProcess::interp()");
    BL_ASSERT(bcr.size() >= ncomp);
    //
    // Make box which is intersection of fine_region and domain of fine.
    //
    Box target_fine_region = fine_region & fine.box();

    Box crse_bx(amrex::coarsen(target_fine_region,ratio));
    Box fslope_bx(amrex::refine(crse_bx,ratio));
    Box cslope_bx(crse_bx);
    cslope_bx.grow(1);
    BL_ASSERT(crse.box().contains(cslope_bx));
    //
    // Alloc temp space for coarse grid slopes: here we use 5
    // instead of AMREX_SPACEDIM because of the x^2, y^2 and xy terms
    //
    long t_long = cslope_bx.numPts();
    BL_ASSERT(t_long < INT_MAX);
    int c_len = int(t_long);

    Vector<Real> cslope(5*c_len);

    int loslp = cslope_bx.index(crse_bx.smallEnd());
    int hislp = cslope_bx.index(crse_bx.bigEnd());

    t_long = cslope_bx.numPts();
    BL_ASSERT(t_long < INT_MAX);
    int cslope_vol = int(t_long);
    int clo        = 1 - loslp;
    int chi        = clo + cslope_vol - 1;
    c_len          = hislp - loslp + 1;
    //
    // Alloc temp space for one strip of fine grid slopes: here we use 5
    // instead of AMREX_SPACEDIM because of the x^2, y^2 and xy terms.
    //
    int dir;
    int f_len = fslope_bx.longside(dir);

    Vector<Real> strip((5+2)*f_len);

    Real* fstrip = strip.dataPtr();
    Real* foff   = fstrip + f_len;
    Real* fslope = foff + f_len;
    //
    // Get coarse and fine edge-centered volume coordinates.
    //
    Vector<Real> fvc[AMREX_SPACEDIM];
    Vector<Real> cvc[AMREX_SPACEDIM];
    for (dir = 0; dir < AMREX_SPACEDIM; dir++)
    {
        fine_geom.GetEdgeVolCoord(fvc[dir],target_fine_region,dir);
        crse_geom.GetEdgeVolCoord(cvc[dir],crse_bx,dir);
    }
    //
    // Alloc tmp space for slope calc and to allow for vectorization.
    //
    Real* fdat        = fine.dataPtr(fine_comp);
    const Real* cdat  = crse.dataPtr(crse_comp);
    const int* flo    = fine.loVect();
    const int* fhi    = fine.hiVect();
    const int* fblo   = target_fine_region.loVect();
    const int* fbhi   = target_fine_region.hiVect();
    const int* cblo   = crse_bx.loVect();
    const int* cbhi   = crse_bx.hiVect();
    const int* fslo   = fslope_bx.loVect();
    const int* fshi   = fslope_bx.hiVect();
    int slope_flag    = (do_limited_slope ? 1 : 0);
    Vector<int> bc     = GetBCArray(bcr);
    const int* ratioV = ratio.getVect();


    amrex_cgpinterp(fdat,AMREX_ARLIM(flo),AMREX_ARLIM(fhi),
                   AMREX_ARLIM(fblo), AMREX_ARLIM(fbhi),
                   &ncomp,AMREX_D_DECL(&ratioV[0],&ratioV[1],&ratioV[2]),
                   cdat,&clo,&chi,
                   AMREX_ARLIM(cblo), AMREX_ARLIM(cbhi),
                   fslo,fshi,
                   cslope.dataPtr(),&c_len,fslope,fstrip,&f_len,foff,
                   bc.dataPtr(), &slope_flag,
                   AMREX_D_DECL(fvc[0].dataPtr(),fvc[1].dataPtr(),fvc[2].dataPtr()),
                   AMREX_D_DECL(cvc[0].dataPtr(),cvc[1].dataPtr(),cvc[2].dataPtr()),
                   &actual_comp,&actual_state);

}

}
