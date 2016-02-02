#ifndef PRECICE_NO_MPI

// Copyright (C) 2011 Technische Universitaet Muenchen
// This file is part of the preCICE project. For conditions of distribution and
// use, please see the license notice at http://www5.in.tum.de/wiki/index.php/PreCICE_License
#include "MVQNPostProcessing.hpp"
#include "cplscheme/CouplingData.hpp"
#include "utils/Globals.hpp"
#include "tarch/la/GramSchmidt.h"
#include "tarch/la/LUDecomposition.h"
#include "tarch/la/MatrixVectorOperations.h"
#include "tarch/la/TransposedMatrix.h"
#include "mesh/Mesh.hpp"
#include "mesh/Vertex.hpp"
#include "utils/Dimensions.hpp"
#include "utils/MasterSlave.hpp"
#include "utils/EventTimings.hpp"
#include "utils/EigenHelperFunctions.hpp"
#include "tarch/la/Scalar.h"
#include "io/TXTWriter.hpp"
#include "io/TXTReader.hpp"
#include "com/MPIPortsCommunication.hpp"
#include "com/Communication.hpp"
#include "Eigen/Dense"

#include <time.h>
#include <sstream>
#include <fstream>
#include <cstring>

#include <thread>
#include <chrono>
//#include "utils/NumericalCompare.hpp"

using precice::utils::Event;


namespace precice {
namespace cplscheme {
namespace impl {

 //tarch::logging::Log MVQNPostProcessing::_log("precice::cplscheme::impl::MVQNPostProcessing");


// ==================================================================================
MVQNPostProcessing:: MVQNPostProcessing
(
  double initialRelaxation,
  bool forceInitialRelaxation,
  int    maxIterationsUsed,
  int    timestepsReused,
  int 	 filter,
  double singularityLimit,
  std::vector<int> dataIDs,
  PtrPreconditioner preconditioner)
:
  BaseQNPostProcessing(initialRelaxation, forceInitialRelaxation, maxIterationsUsed, timestepsReused,
		       filter, singularityLimit, dataIDs, preconditioner),
//  _secondaryOldXTildes(),
  _invJacobian(),
  _oldInvJacobian(),
  _cyclicCommLeft(nullptr),
  _cyclicCommRight(nullptr),
  _parMatrixOps(),
  _alwaysBuildJacobian(false)
{}

// ==================================================================================
MVQNPostProcessing::~MVQNPostProcessing()
{
	//if(utils::MasterSlave::_masterMode ||utils::MasterSlave::_slaveMode){ // not possible because of tests, MasterSlave is deactivated when PP is killed

	// close and shut down cyclic communication connections
	if(_cyclicCommRight != nullptr || _cyclicCommLeft != nullptr){
		if((utils::MasterSlave::_rank % 2) == 0)
		{
		  _cyclicCommLeft->closeConnection();
		  _cyclicCommRight->closeConnection();
		}else{
		  _cyclicCommRight->closeConnection();
		  _cyclicCommLeft->closeConnection();
		}
		_cyclicCommRight = nullptr;
		_cyclicCommLeft = nullptr;
	}
}

// ==================================================================================
void MVQNPostProcessing:: initialize
(
  DataMap& cplData )
{
  preciceTrace(__func__);
  Event e("MVQNPostProcessing::initialize()", true, true); // time measurement, barrier
  // do common QN post processing initialization
  BaseQNPostProcessing::initialize(cplData);
  

  if(utils::MasterSlave::_masterMode ||utils::MasterSlave::_slaveMode){
		/*
		 * TODO: FIXME: This is a temporary and hacky realization of the cyclic commmunication between slaves
		 * 				Therefore the requesterName and accessorName are not given (cf solverInterfaceImpl).
		 * 				The master-slave communication should be modified such that direct communication between
		 * 				slaves is possible (via MPIDirect)
		 */


		 _cyclicCommLeft = com::Communication::SharedPointer(new com::MPIPortsCommunication("."));
		 _cyclicCommRight = com::Communication::SharedPointer(new com::MPIPortsCommunication("."));

		 // initialize cyclic communication between successive slaves
		int prevProc = (utils::MasterSlave::_rank-1 < 0) ? utils::MasterSlave::_size-1 : utils::MasterSlave::_rank-1;
		if((utils::MasterSlave::_rank % 2) == 0)
		{
		  _cyclicCommLeft->acceptConnection("cyclicComm-" + std::to_string(prevProc), "", 0, 1 );
		  _cyclicCommRight->requestConnection("cyclicComm-" +  std::to_string(utils::MasterSlave::_rank), "", 0, 1 );
		}else{
		  _cyclicCommRight->requestConnection("cyclicComm-" +  std::to_string(utils::MasterSlave::_rank), "", 0, 1 );
		  _cyclicCommLeft->acceptConnection("cyclicComm-" + std::to_string(prevProc), "", 0, 1 );
		}
  }

  // initialize parallel matrix-matrix operation module
  _parMatrixOps.initialize(_cyclicCommLeft, _cyclicCommRight);

  int entries = _residuals.size();
  int global_n = 0;

	if (not utils::MasterSlave::_masterMode && not utils::MasterSlave::_slaveMode) {
		global_n = entries;
	}else{
		global_n = _dimOffsets.back();
	}
  
  _invJacobian = Eigen::MatrixXd::Zero(global_n, entries);
  _oldInvJacobian = Eigen::MatrixXd::Zero(global_n, entries);
  _Wtil = Eigen::MatrixXd::Zero(entries, 0);

  _preconditioner->triggerGlobalWeights(global_n);
}

// ==================================================================================
void MVQNPostProcessing::computeUnderrelaxationSecondaryData
(
  DataMap& cplData)
{
    //Store x_tildes for secondary data
  //  foreach (int id, _secondaryDataIDs){
  //    assertion2(_secondaryOldXTildes[id].size() == cplData[id]->values->size(),
  //               _secondaryOldXTildes[id].size(), cplData[id]->values->size());
  //    _secondaryOldXTildes[id] = *(cplData[id]->values);
  //  }

    // Perform underrelaxation with initial relaxation factor for secondary data
    for (int id: _secondaryDataIDs){
      PtrCouplingData data = cplData[id];
      Eigen::VectorXd& values = *(data->values);
      values *= _initialRelaxation;                   // new * omg
      Eigen::VectorXd& secResiduals = _secondaryResiduals[id];
      secResiduals = data->oldValues.col(0);         // old
      secResiduals *= 1.0 - _initialRelaxation;       // (1-omg) * old
      values += secResiduals;                      // (1-omg) * old + new * omg
    }
}

// ==================================================================================
void MVQNPostProcessing::updateDifferenceMatrices
(
    DataMap& cplData)
{
  /**
   *  Matrices and vectors used in this method as well as the result Wtil are not
   *  scaled by the preconditioner.
   */

  preciceTrace(__func__);
  Event e(__func__, true, true); // time measurement, barrier

  // call the base method for common update of V, W matrices
  // important that base method is called before updating _Wtil
  BaseQNPostProcessing::updateDifferenceMatrices(cplData);

  if (_firstIteration && (_firstTimeStep || _forceInitialRelaxation)) {
    // do nothing: constant relaxation
  } else {
    if (not _firstIteration) {
      // Update matrix _Wtil = (W - J_prev*V) with newest information

      Eigen::VectorXd v = _matrixV.col(0);
      Eigen::VectorXd w = _matrixW.col(0);

      // here, we check for _Wtil.cols() as the matrices V, W need to be updated before hand
      // and thus getLSSystemCols() does not yield the correct result.
      bool columnLimitReached = _Wtil.cols() == _maxIterationsUsed;
      bool overdetermined = _Wtil.cols() <= getLSSystemRows();

      Eigen::VectorXd wtil = Eigen::VectorXd::Zero(_matrixV.rows());

      // compute J_prev * V(0) := wtil the new column in _Wtil of dimension: (n x n) * (n x 1) = (n x 1),
      //                                        parallel: (n_global x n_local) * (n_local x 1) = (n_local x 1)
      _parMatrixOps.multiply(_oldInvJacobian, v, wtil, _dimOffsets, getLSSystemRows(), getLSSystemRows(), 1, false);
      wtil = w - wtil;

      if (not columnLimitReached && overdetermined) {
        utils::appendFront(_Wtil, wtil);
      }else {
        utils::shiftSetFirst(_Wtil, wtil);
      }
    }
  }
}


// ==================================================================================
void MVQNPostProcessing::computeQNUpdate
    (PostProcessing::DataMap& cplData, Eigen::VectorXd& xUpdate)
{
  /**
   * The inverse Jacobian
   *
   *        J_inv = J_inv_n + (W - J_inv_n*V)*(V^T*V)^-1*V^T
   *
   * is computed and the resulting quasi-Newton update is returned.
   * Used matrices (V, W, Wtil, invJacobian, oldINvJacobian) are
   * scaled with the used preconditioner.
   */

  preciceTrace(__func__);
  Event e(__func__, true, true); // time measurement, barrier
  preciceDebug("compute IMVJ quasi-Newton update");

  Event ePrecond_1("preconditioning of J", true, true); // ------ time measurement, barrier
  if(_Wtil.size() > 0)
    _preconditioner->apply(_Wtil);
  _preconditioner->apply(_oldInvJacobian,false);
  _preconditioner->revert(_oldInvJacobian,true);
  ePrecond_1.stop();                                    // ------

  computeNewtonUpdateEfficient(cplData, xUpdate);

  Event ePrecond_2("preconditioning of J", true, true); // ------ time measurement, barrier
  if(_Wtil.size() > 0)
    _preconditioner->revert(_Wtil);
  _preconditioner->revert(_oldInvJacobian,false);
  _preconditioner->apply(_oldInvJacobian,true);
  ePrecond_2.stop();                                    // ------
}


// ==================================================================================
void MVQNPostProcessing::pseudoInverse(
    Eigen::MatrixXd& pseudoInverse)
{
  preciceTrace(__func__);
  /**
   *   computation of pseudo inverse matrix Z = (V^TV)^-1 * V^T as solution
   *   to the equation R*z = Q^T(i) for all columns i,  via back substitution.
   */
  auto Q = _qrV.matrixQ();
  auto R = _qrV.matrixR();

  assertion2(pseudoInverse.rows() == _qrV.cols(), pseudoInverse.rows(), _qrV.cols());
  assertion2(pseudoInverse.cols() == _qrV.rows(), pseudoInverse.cols(), _qrV.rows());

  Event e(__func__, true, true); // time measurement, barrier
  Eigen::VectorXd yVec(pseudoInverse.rows());

  // assertions for the case of processors with no vertices
  if(!_hasNodesOnInterface){
      assertion2(_qrV.cols() == getLSSystemCols(), _qrV.cols(), getLSSystemCols()); assertion1(_qrV.rows() == 0, _qrV.rows()); assertion1(Q.size() == 0, Q.size());
  }

  // backsubstitution
  Event e_qr("solve Z = (V^TV)^-1V^T via QR", true, true); // ------- time measurement, barrier
  for (int i = 0; i < Q.rows(); i++) {
    Eigen::VectorXd Qrow = Q.row(i);
    yVec = R.triangularView<Eigen::Upper>().solve<Eigen::OnTheLeft>(Qrow);
    pseudoInverse.col(i) = yVec;
  }
  e_qr.stop();                                            // ----------------
}

// ==================================================================================
void MVQNPostProcessing::buildWtil()
{
  /**
   * PRECONDITION: Assumes that V, W, J_prev are already preconditioned,
   */
  preciceTrace(__func__);
  Event e_WtilV("compute W_til = (W - J_prev*V)", true, true); // time measurement, barrier
  assertion2(_matrixV.rows() == _qrV.rows(), _matrixV.rows(), _qrV.rows());  assertion2(getLSSystemCols() == _qrV.cols(), getLSSystemCols(), _qrV.cols());

  _Wtil = Eigen::MatrixXd::Zero(_qrV.rows(), _qrV.cols());

  // multiply J_prev * V = W_til of dimension: (n x n) * (n x m) = (n x m),
  //                                    parallel:  (n_global x n_local) * (n_local x m) = (n_local x m)
  _parMatrixOps.multiply(_oldInvJacobian, _matrixV, _Wtil, _dimOffsets, getLSSystemRows(), getLSSystemRows(), getLSSystemCols(), false);

  // W_til = (W-J_inv_n*V) = (W-V_tilde)
  _Wtil *= -1.;
  _Wtil = _Wtil + _matrixW;

  _resetLS = false;
  e_WtilV.stop();
}

// ==================================================================================
void MVQNPostProcessing::buildJacobian()
{
  preciceTrace(__func__);
  /**      --- compute inverse Jacobian ---
  *
  * J_inv = J_inv_n + (W - J_inv_n*V)*(V^T*V)^-1*V^T
  *
  * assumes that J_prev, V, W are already preconditioned
  */


  /**
   *  (1) computation of pseudo inverse Z = (V^TV)^-1 * V^T
   */
  Eigen::MatrixXd Z(_qrV.cols(), _qrV.rows());
  pseudoInverse(Z);


  /**
  *  (2) Multiply J_prev * V =: W_tilde
  */
  assertion2(_matrixV.rows() == _qrV.rows(), _matrixV.rows(), _qrV.rows());  assertion2(getLSSystemCols() == _qrV.cols(), getLSSystemCols(), _qrV.cols());
  if(_resetLS){
    buildWtil();
    preciceWarning(__func__," ATTENTION, in buildJacobian call for buildWtill() - this should not be the case except the coupling did only one iteration");
  }

  /** (3) compute invJacobian = W_til*Z
  *
  *  where Z = (V^T*V)^-1*V^T via QR-dec and back-substitution       dimension: (n x n) * (n x m) = (n x m),
  *  and W_til = (W - J_inv_n*V)                                     parallel:  (n_global x n_local) * (n_local x m) = (n_local x m)
  */
  Event e_WtilZ("compute J = W_til*Z", true, true); // -------- time measurement, barrier

  _parMatrixOps.multiply(_Wtil, Z, _invJacobian, _dimOffsets, getLSSystemRows(), getLSSystemCols(), getLSSystemRows());
  e_WtilZ.stop();                                   // --------

  // update Jacobian
  _invJacobian = _invJacobian + _oldInvJacobian;
}

// ==================================================================================
void MVQNPostProcessing::computeNewtonUpdateEfficient(
    PostProcessing::DataMap& cplData,
    Eigen::VectorXd& xUpdate)
{
  preciceTrace(__func__);

  /**      --- update inverse Jacobian efficient, ---
  *   i.e., do not recompute W_til in every iteration and do not build
  *   the entire Jacobian matrix. This is only necessary if the coupling
  *   iteration has converged, namely in the last iteration.
  *
  * J_inv = J_inv_n + (W - J_inv_n*V)*(V^T*V)^-1*V^T
  */

  /**
   *  (1) computation of pseudo inverse Z = (V^TV)^-1 * V^T
   */
  Eigen::MatrixXd Z(_qrV.cols(), _qrV.rows());
  pseudoInverse(Z);

  /**
  *  (2) Construction of _Wtil = (W - J_prev * V), should be already present due to updated computation
  */
  assertion2(_matrixV.rows() == _qrV.rows(), _matrixV.rows(), _qrV.rows());  assertion2(getLSSystemCols() == _qrV.cols(), getLSSystemCols(), _qrV.cols());

  // rebuild matrix Wtil if V changes substantially.
  if(_resetLS){
    buildWtil();
  }

  /**
   *  Avoid computation of Z*Wtil = Jtil \in (n x n). Rather do matrix-vector computations
   *  [ J_prev*(-res) ] + [Wtil * [Z * (-res)] ]
   *  '----- 1 -------'           '----- 2 ----'
   *                      '-------- 3 ---------'
   *
   *  (3) compute r_til = Z*(-residual)   where Z = (V^T*V)^-1*V^T via QR-dec and back-substitution
   *
   *  dimension: (m x n) * (n x 1) = (m x 1),
   *  parallel:  (m x n_local) * (n x 1) = (m x 1)
   */
  Event e_Zr("compute r_til = Z*(-res)", true, true); // -------- time measurement, barrier
  Eigen::VectorXd negativeResiduals = - _residuals;
  Eigen::VectorXd r_til_loc = Eigen::VectorXd::Zero(getLSSystemCols());
  Eigen::VectorXd r_til = Eigen::VectorXd::Zero(getLSSystemCols());

  r_til_loc.noalias() = Z * negativeResiduals;

  // if serial computation on single processor, i.e, no master-slave mode
  if( not utils::MasterSlave::_masterMode && not utils::MasterSlave::_slaveMode){
    r_til = r_til_loc;
  }else{
    utils::MasterSlave::allreduceSum(r_til_loc.data(), r_til.data(), r_til_loc.size());
  }
  e_Zr.stop();                                         // --------

  /**
   * (4) compute _Wtil * r_til
   *
   * dimension: (n x m) * (m x 1) = (n x 1),
   * parallel:  (n_local x m) * (m x 1) = (n_local x 1)
   *
   * Note: r_til is not distributed but locally stored on each proc (dimension m x 1)
   */
  Eigen::VectorXd xUptmp(_residuals.size());
  xUptmp = _Wtil * r_til;                      // local product, result is naturally distributed.

  /**
   *  (5) xUp = J_prev * (-res) + Wtil*Z*(-res)
   */
  Event e_Jpr("compute xUp(1) = J_prev*(-res)", true, true); // -------- time measurement, barrier
  _parMatrixOps.multiply(_oldInvJacobian, negativeResiduals, xUpdate, _dimOffsets, getLSSystemRows(), getLSSystemRows(), 1, false);
  e_Jpr.stop();                                              // --------

  xUpdate += xUptmp;

  // pending deletion: delete Wtil
  if (_firstIteration && _timestepsReused == 0 && not _forceInitialRelaxation) {
    _Wtil.conservativeResize(0,0);
    _resetLS = true;
  }
}

// ==================================================================================
void MVQNPostProcessing::computeNewtonUpdate
(PostProcessing::DataMap& cplData, Eigen::VectorXd& xUpdate)
{
	preciceTrace("computeNewtonFactorsQRDecomposition()");

	/**      --- update inverse Jacobian ---
	*
	* J_inv = J_inv_n + (W - J_inv_n*V)*(V^T*V)^-1*V^T
	*/
  
	/**
   *  (1) computation of pseudo inverse Z = (V^TV)^-1 * V^T
   */
  Eigen::MatrixXd Z(_qrV.cols(), _qrV.rows());
  pseudoInverse(Z);


	/**
	*  (2) Multiply J_prev * V =: W_tilde
	*/
	Event e_WtilV("compute W_til = (W - J_prev*V)", true, true); // time measurement, barrier
	assertion2(_matrixV.rows() == _qrV.rows(), _matrixV.rows(), _qrV.rows());  assertion2(getLSSystemCols() == _qrV.cols(), getLSSystemCols(), _qrV.cols());
	Eigen::MatrixXd W_til = Eigen::MatrixXd::Zero(_qrV.rows(), _qrV.cols());

	// multiply J_prev * V = W_til of dimension: (n x n) * (n x m) = (n x m),
	//                                    parallel:  (n_global x n_local) * (n_local x m) = (n_local x m)
	_parMatrixOps.multiply(_oldInvJacobian, _matrixV, W_til, _dimOffsets, getLSSystemRows(), getLSSystemRows(), getLSSystemCols(), false);


	// W_til = (W-J_inv_n*V) = (W-V_tilde)
	W_til *= -1.;
	W_til = W_til + _matrixW;

	e_WtilV.stop();

	/**
	*  (3) compute invJacobian = W_til*Z
	*
	*  where Z = (V^T*V)^-1*V^T via QR-dec and back-substitution             dimension: (n x n) * (n x m) = (n x m),
	*  and W_til = (W - J_inv_n*V)                                           parallel:  (n_global x n_local) * (n_local x m) = (n_local x m)
	*/
	Event e_WtilZ("compute J = W_til*Z", true, true); // time measurement, barrier

	_parMatrixOps.multiply(W_til, Z, _invJacobian, _dimOffsets, getLSSystemRows(), getLSSystemCols(), getLSSystemRows());
	e_WtilV.stop();

	// update Jacobian
	_invJacobian = _invJacobian + _oldInvJacobian;

	/**
 	 *  (4) solve delta_x = - J_inv * res
	 */
	Event e_up("compute update = J*(-res)", true, true); // -------- time measurement, barrier
	Eigen::VectorXd negativeResiduals = - _residuals;

	// multiply J_inv * (-res) = x_Update of dimension: (n x n) * (n x 1) = (n x 1),
	//                                        parallel: (n_global x n_local) * (n_local x 1) = (n_local x 1)
	_parMatrixOps.multiply(_invJacobian, negativeResiduals, xUpdate, _dimOffsets, getLSSystemRows(), getLSSystemRows(), 1, false);
  e_up.stop();                                         // --------
}

// ==================================================================================
void MVQNPostProcessing:: specializedIterationsConverged
(
   DataMap & cplData)
{
  preciceTrace(__func__);

  // need to apply the preconditioner, as all data structures are reverted after
  // call to computeQNUpdate. Need to call this before the preconditioner is updated.
  Event ePrecond_1("preconditioning of J", true, true); // -------- time measurement, barrier

  _preconditioner->apply(_residuals);
  _preconditioner->apply(_matrixV);
  _preconditioner->apply(_matrixW);

  _preconditioner->apply(_Wtil);
  _preconditioner->apply(_oldInvJacobian,false);
  _preconditioner->revert(_oldInvJacobian,true);

  // should not happen that requireNewQR() is true
  if(_preconditioner->requireNewQR()){
    if(not (_filter==PostProcessing::QR2FILTER)){ //for QR2 filter, there is no need to do this twice
      _qrV.reset(_matrixV, getLSSystemRows());
    }
    _preconditioner->newQRfulfilled();
  }
  ePrecond_1.stop();                                    // --------

  // apply the configured filter to the LS system
  BaseQNPostProcessing::applyFilter();

  // compute explicit representation of Jacobian
  buildJacobian();

  // store inverse Jacobian from last time step
  _oldInvJacobian = _invJacobian;

  Event ePrecond_2("preconditioning of J", true, true); // -------- time measurement, barrier
  _preconditioner->revert(_matrixW);
  _preconditioner->revert(_matrixV);
  _preconditioner->revert(_residuals);

  _preconditioner->revert(_oldInvJacobian,false);
  _preconditioner->apply(_oldInvJacobian,true);
  _preconditioner->revert(_Wtil);
  ePrecond_2.stop();                                    // --------

  /** in case of enforced initial relaxation, the matrices are cleared
   *  in case of timestepsReused > 0, the columns in _Wtil are outdated, as the Jacobian changes, hence clear
   *  in case of timestepsReused == 0 and no initial relaxation, pending deletion in performPostProcessing
   */
  if(_timestepsReused > 0 || (_timestepsReused == 0 && _forceInitialRelaxation)){
    //_Wtil.conservativeResize(0, 0);
    _resetLS = true;
  }
}

// ==================================================================================
void MVQNPostProcessing:: removeMatrixColumn
(
  int columnIndex)
{
  assertion(_matrixV.cols() > 1); assertion(_Wtil.cols() > 1);


  // remove column from matrix _Wtil
  if(not _resetLS)
    utils::removeColumnFromMatrix(_Wtil, columnIndex);

  BaseQNPostProcessing::removeMatrixColumn(columnIndex);
}

}}} // namespace precice, cplscheme, impl

#endif
