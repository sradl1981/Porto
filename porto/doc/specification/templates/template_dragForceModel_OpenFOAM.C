//1 - Some Kind of Header
/*---------------------------------------------------------------------------*\
    CFDEMcoupling - Open Source CFD-DEM coupling

    CFDEMcoupling is part of the CFDEMproject
    www.cfdem.com
                                Christoph Goniva, christoph.goniva@cfdem.com
                                Copyright 2009-2012 JKU Linz
                                Copyright 2012-     DCS Computing GmbH, Linz
                                Copyright 2013-     Graz University of Technology
-------------------------------------------------------------------------------
License
    This file is part of CFDEMcoupling.

    CFDEMcoupling is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the
    Free Software Foundation; either version 3 of the License, or (at your
    option) any later version.

    CFDEMcoupling is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with CFDEMcoupling; if not, write to the Free Software Foundation,
    Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

Description
    This code is designed to realize coupled CFD-DEM simulations using LIGGGHTS
    and OpenFOAM(R). Note: this code is not part of OpenFOAM(R) (see DISCLAIMER).
\*---------------------------------------------------------------------------*/

// 2 - Various includes, typically one is specific for a certain model
#include "error.H"

#include "BeetstraDrag.H"


#include "addToRunTimeSelectionTable.H"


// 3 - The actual implementation

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace Foam
{

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

// 3 - A - Here we define the name and handles that the model is selectable at runtime

defineTypeNameAndDebug(BeetstraDrag, 0);

addToRunTimeSelectionTable
(
    forceModel,
    BeetstraDrag,
    dictionary
);


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

// 3 - B - A Typical Constructor, this one also sets some basic quantities
//          important for the evaluation  

// Construct from components
BeetstraDrag::BeetstraDrag
(
    const dictionary& dict,
    cfdemCloud& sm
)
:
    forceModel(dict,sm),
    propsDict_(dict.subDict(typeName + "Props")),
    verbose_(false),
    velFieldName_(propsDict_.lookup("velFieldName")),
    U_(sm.mesh().lookupObject<volVectorField> (velFieldName_)),
    densityFieldName_(propsDict_.lookup("densityFieldName")),
    rho_(sm.mesh().lookupObject<volScalarField> (densityFieldName_)),
    voidfractionFieldName_(propsDict_.lookup("voidfractionFieldName")),
    voidfraction_(sm.mesh().lookupObject<volScalarField> (voidfractionFieldName_)),
    gravityFieldName_(propsDict_.lookup("gravityFieldName")),
    #if defined(version21) || defined(version16ext)
        g_(sm.mesh().lookupObject<uniformDimensionedVectorField> (gravityFieldName_)),
    #elif defined(version15)
        g_(dimensionedVector(sm.mesh().lookupObject<IOdictionary>("environmentalProperties").lookup(gravityFieldName_)).value()),
    #endif
    dPrim_(1.0),
    rhoParticle_(0.0),
    interpolation_(false),
    scale_(1.),
    useFilteredDragModel_(false),
    useParcelSizeDependentFilteredDrag_(false),
    filtDragParamsK_(0.), 
    filtDragParamsALimit_(0.),
    filtDragParamsAExponent_(1.0),
    filtDragParamsLChar2_(0.0),
    basicCalculator_()
{
    if (propsDict_.found("verbose")) verbose_=true;
    basicCalculator_.verbose_=verbose_;
    if (propsDict_.found("treatExplicit")) treatExplicit_=true;
    if (propsDict_.found("interpolation")) interpolation_=true;
    if (propsDict_.found("implDEM"))
    {
        treatExplicit_=false;
        implDEM_=true;
        setImpDEMdrag();
        Info << "Using implicit DEM drag formulation." << endl;
    }
    if (propsDict_.found("useFilteredDragModel")) 
    {
        useFilteredDragModel_ = true;
        rhoParticle_= readScalar(propsDict_.lookup("rhoParticle"));
    }
    if (propsDict_.found("useParcelSizeDependentFilteredDrag")) 
    {
        useParcelSizeDependentFilteredDrag_=true;
        filtDragParamsK_ = readScalar(propsDict_.lookup("k")); 
        filtDragParamsALimit_ = readScalar(propsDict_.lookup("aLimit"));
        filtDragParamsAExponent_ = readScalar(propsDict_.lookup("aExponent"));
    }
         
    if(useParcelSizeDependentFilteredDrag_)   
    { 
        useFilteredDragModel_ = true;
        Info << "Beetstra drag model: forced the use of the filtered drag model\n";
    }
    particleCloud_.checkCG(true);

    //Set the reference length scale from particle information
    // get viscosity field
    #ifdef comp
        const volScalarField nufField = particleCloud_.turbulence().mu()/rho_;
    #else
        const volScalarField& nufField = particleCloud_.turbulence().nu();
    #endif
    scalar nuf(0);
    scalar rho(0);

    if (useFilteredDragModel_) 
    {
                //select viscosity and density
                //based on first value in the field
                nuf = nufField[0];
                rho = rho_[0];
                dPrim_ = readScalar(propsDict_.lookup("dPrim"));
                basicCalculator_.setupSettling
                (
                        dPrim_, 
                        rhoParticle_,
                        mag(g_).value(),
                        nuf*rho,
                        rho,
                        1 //drag law: 1...Beetstra
                );
                filtDragParamsLChar2_ = basicCalculator_.settling.Lchar2;
                {
                        Info << "dPrim_: " 
                             << dPrim_ << tab << "[m]" << endl;
                        Info << "Reference length for filtered drag computations: " 
                             << filtDragParamsLChar2_ << tab << "[m]" << endl;
                }
    }

    //Append the field names to be probed
    particleCloud_.probeM().initialize(typeName, "beetstraDrag.logDat");
    particleCloud_.probeM().vectorFields_.append("dragForce"); //first entry must the be the force
    particleCloud_.probeM().vectorFields_.append("Urel");        //other are debug
    particleCloud_.probeM().scalarFields_.append("Rep");          //other are debug
    particleCloud_.probeM().scalarFields_.append("beta");                 //other are debug
    particleCloud_.probeM().scalarFields_.append("voidfraction");       //other are debug
    particleCloud_.probeM().scalarFields_.append("filterDragPrefactor");          //other are debug
    particleCloud_.probeM().writeHeader();

}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

BeetstraDrag::~BeetstraDrag()
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

// 3 - C - The force calculation function


void BeetstraDrag::setForce() const
{
    scale_=cg();
    Info << "BeetstraDrag using scale from liggghts cg = " << scale_ << endl;

    // get viscosity field
    #ifdef comp
        const volScalarField nufField = particleCloud_.turbulence().mu()/rho_;
    #else
        const volScalarField& nufField = particleCloud_.turbulence().nu();
    #endif

    vector position(0,0,0);
    scalar voidfraction(1);
    vector Ufluid(0,0,0);
    vector drag(0,0,0);
    label cellI=0;
    scalar beta(0);

    vector Us(0,0,0);
    vector Ur(0,0,0);
    scalar ds(0);
    scalar nuf(0);
    scalar rho(0);
    scalar magUr(0);
    scalar Rep(0);
	scalar Vs(0);
	scalar localPhiP(0);
	scalar filterDragPrefactor(1.0);
	scalar cCorrParcelSize_(1.0) ;
	scalar vCell(0);
	scalar FfFilterPrime(1);
	
    scalar F0=0.;
    scalar G0=0.;

    interpolationCellPoint<scalar> voidfractionInterpolator_(voidfraction_);
    interpolationCellPoint<vector> UInterpolator_(U_);

    #include "setupProbeModel.H"

    for(int index = 0;index <  particleCloud_.numberOfParticles(); index++)
    {
        //if(mask[index][0])
        //{
            cellI = particleCloud_.cellIDs()[index][0];
            drag = vector(0,0,0);
            Ufluid= vector(0,0,0);
            
            if (cellI > -1) // particle Found
            {
                if(interpolation_)
                {
	                position     = particleCloud_.position(index);
                    voidfraction = voidfractionInterpolator_.interpolate(position,cellI);
                    Ufluid       = UInterpolator_.interpolate(position,cellI);
                    //Ensure interpolated void fraction to be meaningful
                    // Info << " --> voidfraction: " << voidfraction << endl;
                    if(voidfraction>1.00) voidfraction = 1.00;
                    if(voidfraction<0.10) voidfraction = 0.10;
                }
                else
                {
					voidfraction = voidfraction_[cellI];
                    Ufluid = U_[cellI];
                }
           
                Us = particleCloud_.velocity(index);
                Ur = Ufluid-Us;
                ds = 2*particleCloud_.radius(index);
                dPrim_ = ds/scale_;
                nuf = nufField[cellI];
                rho = rho_[cellI];

                magUr = mag(Ur);
				Rep = 0;
                Vs = ds*ds*ds*M_PI/6;
                localPhiP = 1-voidfraction+SMALL;
                vCell = U_.mesh().V()[cellI];
                
                 if (magUr > 0)
                {
                    // calc particle Re Nr
                    Rep = dPrim_*voidfraction*magUr/(nuf+SMALL);


// 3 - C - 1 - In this section we could insert parameters
//             or functions
//             that come from a database

                    // calc model coefficient F0
                    F0 = 10.f * localPhiP / voidfraction / voidfraction
                       + voidfraction * voidfraction * ( 1.0+1.5*sqrt(localPhiP) );

                    // calc model coefficient G0
                    G0 =  0.01720833 * Rep / voidfraction / voidfraction  //0.0172083 = 0.413/24
                              *  ( 1.0 / voidfraction + 3.0 * localPhiP  * voidfraction + 8.4 
                                                            * powf(Rep,-0.343) ) 
                              /  ( 1.0 + powf( 10., 3.0* localPhiP ) 
                                       * powf( Rep,-(1.0+4.0*localPhiP)/2.0 ) );

                    // calc model coefficient beta
                    beta =  18.*nuf*rho/(dPrim_*dPrim_)
                              *voidfraction
                              *(F0 + G0);

                    //Apply filtered drag correction
                    if(useFilteredDragModel_)
                    {
                        FfFilterPrime =  FfFilterFunc(
                                                    filtDragParamsLChar2_,
                                                    vCell,
                                                    0
                                                   );
                 
                        filterDragPrefactor = 1 - fFuncFilteredDrag(FfFilterPrime, localPhiP)
                                                * hFuncFilteredDrag(localPhiP);
                                           
                        beta *= filterDragPrefactor;
                    }
                    if(useParcelSizeDependentFilteredDrag_) //Apply filtered drag correction
                    {
                        scalar dParceldPrim = scale_;
                        cCorrParcelSize_ = 
                               cCorrFunctionFilteredDrag(
                                          filtDragParamsK_, 
                                          filtDragParamsALimit_,
                                          filtDragParamsAExponent_,
                                          localPhiP,
                                          dParceldPrim
                                       );                                          
                        beta *= cCorrParcelSize_;
                    }

                    // calc particle's drag
                    drag = Vs * beta * Ur;

                    if (modelType_=="B")
                        drag /= voidfraction;
                }

// 3 - C - 2 - This is a standardized debug and report section

                if( verbose_ ) //&& index>100 && index < 105)
                {
                    Info << "index / cellI = " << index << tab << cellI << endl;
                    Info << "position = " << particleCloud_.position(index) << endl;
                    Info << "Us = " << Us << endl;
                    Info << "Ur = " << Ur << endl;
                    Info << "ds = " << ds << endl;
                    Info << "rho = " << rho << endl;
                    Info << "nuf = " << nuf << endl;
                    Info << "voidfraction = " << voidfraction << endl;
                    Info << "voidfraction_[cellI]: " << voidfraction_[cellI] << endl;
                    Info << "Rep = " << Rep << endl;
                    Info << "filterDragPrefactor = " << filterDragPrefactor << endl;
                    Info << "fFuncFilteredDrag: " << fFuncFilteredDrag(FfFilterPrime, localPhiP) << endl;
                    Info << "hFuncFilteredDrag: " << hFuncFilteredDrag(localPhiP) << endl;
                    Info << "cCorrParcelSize:   " << cCorrParcelSize_ << endl;
                    Info << "F0 / G0: " << F0 << tab << G0 << endl;
                    Info << "beta:   " << beta << endl;
                    Info << "drag = " << drag << endl;

                    Info << "\nBeetstra drag model settings: treatExplicit " << treatExplicit_ << tab
                                    << "verbose: " << verbose_ << tab
                                    << "dPrim: " << dPrim_  << tab
                                    << "interpolation: " << interpolation_  << tab
                                    << "filteredDragM: " << useFilteredDragModel_   << tab
                                    << "parcelSizeDepDrag: " << useParcelSizeDependentFilteredDrag_
                                    << endl << endl;
                }

                //Set value fields and write the probe
                if(probeIt_)
                {
                    #include "setupProbeModelfields.H"
                    vValues.append(drag);           //first entry must the be the force
                    vValues.append(Ur);                
                    sValues.append(Rep);
                    sValues.append(beta);
                    sValues.append(voidfraction);
                    sValues.append(filterDragPrefactor);
                    particleCloud_.probeM().writeProbe(index, sValues, vValues);
                }
            }

            // set force on particle
            if(treatExplicit_) for(int j=0;j<3;j++) expForces()[index][j] += drag[j];
            else  for(int j=0;j<3;j++) impForces()[index][j] += drag[j];

            // set Cd
            if(implDEM_)
            {
                for(int j=0;j<3;j++) fluidVel()[index][j]=Ufluid[j];

                if (modelType_=="B")
                    Cds()[index][0] = Vs*beta/voidfraction;
                else
                    Cds()[index][0] = Vs*beta;

            }else{
                for(int j=0;j<3;j++) DEMForces()[index][j] += drag[j];
            }
        //}
    }
}

#include "./filteredDragFunctions/filteredDragFunctions.C"


// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

} // End namespace Foam

// ************************************************************************* //
