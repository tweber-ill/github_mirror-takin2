/**
 * tlibs2 -- magnetic dynamics -- spin-spin correlation
 * @author Tobias Weber <tweber@ill.fr>
 * @date 2022 - 2024
 * @license GPLv3, see 'LICENSE' file
 *
 * References:
 *   - (Toth 2015) S. Toth and B. Lake, J. Phys.: Condens. Matter 27 166002 (2015):
 *                 https://doi.org/10.1088/0953-8984/27/16/166002
 *                 https://arxiv.org/abs/1402.6069
 *   - (Heinsdorf 2021) N. Heinsdorf, manual example calculation for a simple
 *                      ferromagnetic case, personal communications, 2021/2022.
 *
 * @desc This file implements the formalism given by (Toth 2015).
 * @desc For further references, see the 'LITERATURE' file.
 *
 * ----------------------------------------------------------------------------
 * tlibs
 * Copyright (C) 2017-2024  Tobias WEBER (Institut Laue-Langevin (ILL),
 *                          Grenoble, France).
 * Copyright (C) 2015-2017  Tobias WEBER (Technische Universitaet Muenchen
 *                          (TUM), Garching, Germany).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * ----------------------------------------------------------------------------
 */

#ifndef __TLIBS2_MAGDYN_CORREL_H__
#define __TLIBS2_MAGDYN_CORREL_H__

#include <algorithm>
#include <numeric>
#include <vector>
#include <array>
#include <tuple>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <string_view>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <thread>
#include <cstdint>

#include <boost/asio.hpp>
#include <boost/container_hash/hash.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include "../maths.h"
#include "../units.h"
#include "../phys.h"
#include "../algos.h"
#include "../expr.h"

#include "magdyn.h"


// --------------------------------------------------------------------
// calculation functions
// --------------------------------------------------------------------

/**
 * get the dynamical structure factor from a hamiltonian
 * @note implements the formalism given by (Toth 2015)
 */
MAGDYN_TEMPL
void MAGDYN_INST::CalcCorrelationsFromHamiltonian(
	MAGDYN_TYPE::EnergiesAndWeights& energies_and_correlations,
	const t_mat& H_mat, const t_mat& chol_mat, const t_mat& g_sign,
	const t_vec_real& Qvec, const std::vector<t_vec>& evecs) const
{
	const t_size N = GetMagneticSitesCount();
	if(N == 0)
		return;

	// get the sorting of the energies
	const std::vector<t_size> sorting = tl2::get_perm(
		energies_and_correlations.size(),
		[&energies_and_correlations](t_size idx1, t_size idx2) -> bool
	{
		return energies_and_correlations[idx1].E >=
			energies_and_correlations[idx2].E;
	});

	const t_mat evec_mat = tl2::create<t_mat>(tl2::reorder(evecs, sorting));
	const t_mat evec_mat_herm = tl2::herm(evec_mat);

	// equation (32) from (Toth 2015)
	const t_mat energy_mat = evec_mat_herm * H_mat * evec_mat;  // energies
	t_mat E_sqrt = g_sign * energy_mat;                         // abs. energies
	for(t_size i = 0; i < E_sqrt.size1(); ++i)
		E_sqrt(i, i) = std::sqrt(E_sqrt(i, i));             // sqrt. of abs. energies

	// re-create energies, to be consistent with the weights
	energies_and_correlations.clear();
	for(t_size i = 0; i < energy_mat.size1(); ++i)
	{
		const EnergyAndWeight EandS
		{
			.E = energy_mat(i, i).real(),
			.S = tl2::zero<t_mat>(3, 3),
			.S_perp = tl2::zero<t_mat>(3, 3),
		};

		energies_and_correlations.emplace_back(std::move(EandS));
	}

	const auto [chol_inv, inv_ok] = tl2::inv(chol_mat);
	if(!inv_ok)
	{
		using namespace tl2_ops;
		std::cerr << "Magdyn warning: Inversion failed"
			<< " at Q = " << Qvec << "." << std::endl;
	}

	// equation (34) from (Toth 2015)
	const t_mat trafo = chol_inv * evec_mat * E_sqrt;
	const t_mat trafo_herm = tl2::herm(trafo);

#ifdef __TLIBS2_MAGDYN_DEBUG_OUTPUT__
	t_mat D_mat = trafo_herm * H_mat * trafo;
	std::cout << "D = \n";
	tl2::niceprint(std::cout, D_mat, 1e-4, 4);
	std::cout << "\nE = \n";
	tl2::niceprint(std::cout, E_sqrt, 1e-4, 4);
	std::cout << "\nL = \n";
	tl2::niceprint(std::cout, energy_mat, 1e-4, 4);
	std::cout << std::endl;
#endif

	// building the spin correlation functions of equation (47) from (Toth 2015)
	for(std::uint8_t x_idx = 0; x_idx < 3; ++x_idx)
	for(std::uint8_t y_idx = 0; y_idx < 3; ++y_idx)
	{
		// equations (44) from (Toth 2015)
		t_mat M00 = tl2::create<t_mat>(N, N);
		t_mat M0N = tl2::create<t_mat>(N, N);
		t_mat MN0 = tl2::create<t_mat>(N, N);
		t_mat MNN = tl2::create<t_mat>(N, N);

		for(t_size i = 0; i < N; ++i)
		for(t_size j = 0; j < N; ++j)
		{
			// get the sites
			const MagneticSite& s_i = GetMagneticSite(i);
			const MagneticSite& s_j = GetMagneticSite(j);

			// get the pre-calculated u vectors
			const t_vec& u_i = s_i.ge_trafo_plane_calc;
			const t_vec& u_j = s_j.ge_trafo_plane_calc;
			const t_vec& uc_i = s_i.ge_trafo_plane_conj_calc;
			const t_vec& uc_j = s_j.ge_trafo_plane_conj_calc;

			// pre-factors of equation (44) from (Toth 2015)
			const t_real S_mag = std::sqrt(s_i.spin_mag_calc * s_j.spin_mag_calc);
			const t_cplx phase = std::exp(-m_phase_sign * s_imag * s_twopi *
				tl2::inner<t_vec_real>(s_j.pos_calc - s_i.pos_calc, Qvec));

			// matrix elements of equation (44) from (Toth 2015)
			M00(i, j) = phase * S_mag * u_i[x_idx]  * uc_j[y_idx];
			M0N(i, j) = phase * S_mag * u_i[x_idx]  * u_j[y_idx];
			MN0(i, j) = phase * S_mag * uc_i[x_idx] * uc_j[y_idx];
			MNN(i, j) = phase * S_mag * uc_i[x_idx] * u_j[y_idx];
		} // end of iteration over sites

		// equation (47) from (Toth 2015)
		t_mat M = tl2::create<t_mat>(2*N, 2*N);
		tl2::set_submat(M, M00, 0, 0); tl2::set_submat(M, M0N, 0, N);
		tl2::set_submat(M, MN0, N, 0); tl2::set_submat(M, MNN, N, N);

		const t_mat M_trafo = trafo_herm * M * trafo;

#ifdef __TLIBS2_MAGDYN_DEBUG_OUTPUT__
		std::cout << "M_trafo for x=" << x_idx << ", y=" << y_idx << ":\n";
		tl2::niceprint(std::cout, M_trafo, 1e-4, 4);
		std::cout << std::endl;
#endif

		for(t_size i = 0; i < energies_and_correlations.size(); ++i)
		{
			(energies_and_correlations[i].S)(x_idx, y_idx) +=
				M_trafo(i, i) / t_real(M.size1());
		}
	} // end of coordinate iteration
}



/**
 * applies projectors, form and weight factors to get neutron intensities
 * @note implements the formalism given by (Toth 2015)
 */
MAGDYN_TEMPL
void MAGDYN_INST::CalcIntensities(const t_vec_real& Q_rlu,
	MAGDYN_TYPE::EnergiesAndWeights& energies_and_correlations) const
{
	using namespace tl2_ops;
	tl2::ExprParser<t_cplx> magffact = m_magffact;

	for(EnergyAndWeight& E_and_S : energies_and_correlations)
	{
		// apply bose factor
		if(m_temperature >= 0.)
			E_and_S.S *= tl2::bose_cutoff(E_and_S.E, m_temperature, m_bose_cutoff);

		// apply form factor
		if(m_magffact_formula != "")
		{
			// get |Q| in units of A^(-1)
			t_vec_real Q_invA = m_xtalB * Q_rlu;
			t_real Q_abs = tl2::norm<t_vec_real>(Q_invA);

			// evaluate form factor expression
			magffact.register_var("Q", Q_abs);
			t_real ffact = magffact.eval_noexcept().real();
			E_and_S.S *= ffact;
		}

		// apply orthogonal projector for magnetic neutron scattering,
		// see (Shirane 2002), p. 37, equation (2.64)
		t_mat proj_neutron = tl2::ortho_projector<t_mat, t_vec>(Q_rlu, false);
		E_and_S.S_perp = proj_neutron * E_and_S.S * proj_neutron;

		// weights
		E_and_S.S_sum       = tl2::trace<t_mat>(E_and_S.S);
		E_and_S.S_perp_sum  = tl2::trace<t_mat>(E_and_S.S_perp);
		E_and_S.weight_full = std::abs(E_and_S.S_sum.real());
		E_and_S.weight      = std::abs(E_and_S.S_perp_sum.real());

		// TODO: polarisation via blume-maleev equation
		/*static const t_vec_real h_rlu = tl2::create<t_vec_real>({ 1., 0., 0. });
		static const t_vec_real l_rlu = tl2::create<t_vec_real>({ 0., 0., 1. });
		t_vec_real h_lab = m_xtalUB * h_rlu;
		t_vec_real l_lab = m_xtalUB * l_rlu;
		t_vec_real Q_lab = m_xtalUB * Q_rlu;
		t_mat_real rotQ = tl2::rotation<t_mat_real>(h_lab, Q_lab, &l_lab, m_eps, true);
		t_mat_real rotQ_hkl = m_xtalUBinv * rotQ * m_xtalUB;
		const auto [rotQ_hkl_inv, rotQ_hkl_inv_ok] = tl2::inv(rotQ_hkl);
		if(!rotQ_hkl_inv_ok)
			std::cerr << "Magdyn error: Cannot invert Q rotation matrix."
				<< std::endl;

		t_mat rotQ_hkl_cplx = tl2::convert<t_mat, t_mat_real>(rotQ_hkl);
		t_mat rotQ_hkl_inv_cplx = tl2::convert<t_mat, t_mat_real>(rotQ_hkl_inv);

		t_mat S_perp_pol = rotQ_hkl_cplx * E_and_S.S_perp * rotQ_hkl_inv_cplx;
		t_mat S_pol = rotQ_hkl_cplx * E_and_S.S * rotQ_hkl_inv_cplx;*/
	}
}
// --------------------------------------------------------------------

#endif