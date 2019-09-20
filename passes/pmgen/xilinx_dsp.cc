/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *                2019  Eddie Hung    <eddie@fpgeh.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

bool did_something;

#include "passes/pmgen/xilinx_dsp_pm.h"
#include "passes/pmgen/xilinx_dsp_cascade_pm.h"

static Cell* addDsp(Module *module) {
	Cell *cell = module->addCell(NEW_ID, ID(DSP48E1));
	cell->setParam(ID(ACASCREG), 0);
	cell->setParam(ID(ADREG), 0);
	cell->setParam(ID(A_INPUT), Const("DIRECT"));
	cell->setParam(ID(ALUMODEREG), 0);
	cell->setParam(ID(AREG), 0);
	cell->setParam(ID(BCASCREG), 0);
	cell->setParam(ID(B_INPUT), Const("DIRECT"));
	cell->setParam(ID(BREG), 0);
	cell->setParam(ID(CARRYINREG), 0);
	cell->setParam(ID(CARRYINSELREG), 0);
	cell->setParam(ID(CREG), 0);
	cell->setParam(ID(DREG), 0);
	cell->setParam(ID(INMODEREG), 0);
	cell->setParam(ID(MREG), 0);
	cell->setParam(ID(OPMODEREG), 0);
	cell->setParam(ID(PREG), 0);
	cell->setParam(ID(USE_MULT), Const("NONE"));
	cell->setParam(ID(USE_SIMD), Const("ONE48"));
	cell->setParam(ID(USE_DPORT), Const("FALSE"));

	cell->setPort(ID(D), Const(0, 25));
	cell->setPort(ID(INMODE), Const(0, 5));
	cell->setPort(ID(ALUMODE), Const(0, 4));
	cell->setPort(ID(OPMODE), Const(0, 7));
	cell->setPort(ID(CARRYINSEL), Const(0, 3));
	cell->setPort(ID(ACIN), Const(0, 30));
	cell->setPort(ID(BCIN), Const(0, 18));
	cell->setPort(ID(PCIN), Const(0, 48));
	cell->setPort(ID(CARRYIN), Const(0, 1));
	return cell;
}

void pack_xilinx_simd(Module *module, const std::vector<Cell*> &selected_cells)
{
	std::deque<Cell*> simd12_add, simd12_sub;
	std::deque<Cell*> simd24_add, simd24_sub;

	for (auto cell : selected_cells) {
		if (!cell->type.in(ID($add), ID($sub)))
			continue;
		SigSpec Y = cell->getPort(ID(Y));
		if (!Y.is_chunk())
			continue;
		if (!Y.as_chunk().wire->get_strpool_attribute(ID(use_dsp)).count("simd"))
			continue;
		if (GetSize(Y) > 25)
			continue;
		SigSpec A = cell->getPort(ID(A));
		SigSpec B = cell->getPort(ID(B));
		if (GetSize(Y) <= 13) {
			if (GetSize(A) > 12)
				continue;
			if (GetSize(B) > 12)
				continue;
			if (cell->type == ID($add))
				simd12_add.push_back(cell);
			else if (cell->type == ID($sub))
				simd12_sub.push_back(cell);
		}
		else if (GetSize(Y) <= 25) {
			if (GetSize(A) > 24)
				continue;
			if (GetSize(B) > 24)
				continue;
			if (cell->type == ID($add))
				simd24_add.push_back(cell);
			else if (cell->type == ID($sub))
				simd24_sub.push_back(cell);
		}
		else
			log_abort();
	}

	auto f12 = [module](SigSpec &AB, SigSpec &C, SigSpec &P, SigSpec &CARRYOUT, Cell *lane) {
		SigSpec A = lane->getPort(ID(A));
		SigSpec B = lane->getPort(ID(B));
		SigSpec Y = lane->getPort(ID(Y));
		A.extend_u0(12, lane->getParam(ID(A_SIGNED)).as_bool());
		B.extend_u0(12, lane->getParam(ID(B_SIGNED)).as_bool());
		AB.append(A);
		C.append(B);
		if (GetSize(Y) < 13)
			Y.append(module->addWire(NEW_ID, 13-GetSize(Y)));
		else
			log_assert(GetSize(Y) == 13);
		P.append(Y.extract(0, 12));
		CARRYOUT.append(Y[12]);
	};
	auto g12 = [&f12,module](std::deque<Cell*> &simd12) {
		while (simd12.size() > 1) {
			SigSpec AB, C, P, CARRYOUT;

			Cell *lane1 = simd12.front();
			simd12.pop_front();
			Cell *lane2 = simd12.front();
			simd12.pop_front();
			Cell *lane3 = nullptr;
			Cell *lane4 = nullptr;

			if (!simd12.empty()) {
				lane3 = simd12.front();
				simd12.pop_front();
				if (!simd12.empty()) {
					lane4 = simd12.front();
					simd12.pop_front();
				}
			}

			log("Analysing %s.%s for Xilinx DSP SIMD12 packing.\n", log_id(module), log_id(lane1));

			Cell *cell = addDsp(module);
			cell->setParam(ID(USE_SIMD), Const("FOUR12"));
			// X = A:B
			// Y = 0
			// Z = C
			cell->setPort(ID(OPMODE), Const::from_string("0110011"));

			log_assert(lane1);
			log_assert(lane2);
			f12(AB, C, P, CARRYOUT, lane1);
			f12(AB, C, P, CARRYOUT, lane2);
			if (lane3) {
				f12(AB, C, P, CARRYOUT, lane3);
				if (lane4)
					f12(AB, C, P, CARRYOUT, lane4);
				else {
					AB.append(Const(0, 12));
					C.append(Const(0, 12));
					P.append(module->addWire(NEW_ID, 12));
					CARRYOUT.append(module->addWire(NEW_ID, 1));
				}
			}
			else {
				AB.append(Const(0, 24));
				C.append(Const(0, 24));
				P.append(module->addWire(NEW_ID, 24));
				CARRYOUT.append(module->addWire(NEW_ID, 2));
			}
			log_assert(GetSize(AB) == 48);
			log_assert(GetSize(C) == 48);
			log_assert(GetSize(P) == 48);
			log_assert(GetSize(CARRYOUT) == 4);
			cell->setPort(ID(A), AB.extract(18, 30));
			cell->setPort(ID(B), AB.extract(0, 18));
			cell->setPort(ID(C), C);
			cell->setPort(ID(P), P);
			cell->setPort(ID(CARRYOUT), CARRYOUT);
			if (lane1->type == ID($sub))
				cell->setPort(ID(ALUMODE), Const::from_string("0011"));

			module->remove(lane1);
			module->remove(lane2);
			if (lane3) module->remove(lane3);
			if (lane4) module->remove(lane4);

			module->design->select(module, cell);
		}
	};
	g12(simd12_add);
	g12(simd12_sub);

	auto f24 = [module](SigSpec &AB, SigSpec &C, SigSpec &P, SigSpec &CARRYOUT, Cell *lane) {
		SigSpec A = lane->getPort(ID(A));
		SigSpec B = lane->getPort(ID(B));
		SigSpec Y = lane->getPort(ID(Y));
		A.extend_u0(24, lane->getParam(ID(A_SIGNED)).as_bool());
		B.extend_u0(24, lane->getParam(ID(B_SIGNED)).as_bool());
		C.append(A);
		AB.append(B);
		if (GetSize(Y) < 25)
			Y.append(module->addWire(NEW_ID, 25-GetSize(Y)));
		else
			log_assert(GetSize(Y) == 25);
		P.append(Y.extract(0, 24));
		CARRYOUT.append(module->addWire(NEW_ID)); // TWO24 uses every other bit
		CARRYOUT.append(Y[24]);
	};
	auto g24 = [&f24,module](std::deque<Cell*> &simd24) {
		while (simd24.size() > 1) {
			SigSpec AB;
			SigSpec C;
			SigSpec P;
			SigSpec CARRYOUT;

			Cell *lane1 = simd24.front();
			simd24.pop_front();
			Cell *lane2 = simd24.front();
			simd24.pop_front();

			log("Analysing %s.%s for Xilinx DSP SIMD24 packing.\n", log_id(module), log_id(lane1));

			Cell *cell = addDsp(module);
			cell->setParam(ID(USE_SIMD), Const("TWO24"));
			// X = A:B
			// Y = 0
			// Z = C
			cell->setPort(ID(OPMODE), Const::from_string("0110011"));

			log_assert(lane1);
			log_assert(lane2);
			f24(AB, C, P, CARRYOUT, lane1);
			f24(AB, C, P, CARRYOUT, lane2);
			log_assert(GetSize(AB) == 48);
			log_assert(GetSize(C) == 48);
			log_assert(GetSize(P) == 48);
			log_assert(GetSize(CARRYOUT) == 4);
			cell->setPort(ID(A), AB.extract(18, 30));
			cell->setPort(ID(B), AB.extract(0, 18));
			cell->setPort(ID(C), C);
			cell->setPort(ID(P), P);
			cell->setPort(ID(CARRYOUT), CARRYOUT);
			if (lane1->type == ID($sub))
				cell->setPort(ID(ALUMODE), Const::from_string("0011"));

			module->remove(lane1);
			module->remove(lane2);

			module->design->select(module, cell);
		}
	};
	g24(simd24_add);
	g24(simd24_sub);
}


void pack_xilinx_dsp(xilinx_dsp_pm &pm)
{
	auto &st = pm.st_xilinx_dsp_pack;

#if 1
	log("\n");
	log("preAdd:     %s\n", log_id(st.preAdd, "--"));
	log("ffAD:       %s %s %s\n", log_id(st.ffAD, "--"), log_id(st.ffADcemux, "--"), log_id(st.ffADrstmux, "--"));
	log("ffA2:       %s %s %s\n", log_id(st.ffA2, "--"), log_id(st.ffA2cemux, "--"), log_id(st.ffA2rstmux, "--"));
	log("ffA1:       %s %s %s\n", log_id(st.ffA1, "--"), log_id(st.ffA1cemux, "--"), log_id(st.ffA1rstmux, "--"));
	log("ffB2:       %s %s %s\n", log_id(st.ffB2, "--"), log_id(st.ffB2cemux, "--"), log_id(st.ffB2rstmux, "--"));
	log("ffB1:       %s %s %s\n", log_id(st.ffB1, "--"), log_id(st.ffB1cemux, "--"), log_id(st.ffB1rstmux, "--"));
	log("ffC:        %s %s %s\n", log_id(st.ffC, "--"), log_id(st.ffCcemux, "--"), log_id(st.ffCrstmux, "--"));
	log("ffD:        %s %s %s\n", log_id(st.ffD, "--"), log_id(st.ffDcemux, "--"), log_id(st.ffDrstmux, "--"));
	log("dsp:        %s\n", log_id(st.dsp, "--"));
	log("ffM:        %s %s %s\n", log_id(st.ffM, "--"), log_id(st.ffMcemux, "--"), log_id(st.ffMrstmux, "--"));
	log("postAdd:    %s\n", log_id(st.postAdd, "--"));
	log("postAddMux: %s\n", log_id(st.postAddMux, "--"));
	log("ffP:        %s %s %s\n", log_id(st.ffP, "--"), log_id(st.ffPcemux, "--"), log_id(st.ffPrstmux, "--"));
	log("overflow:   %s\n", log_id(st.overflow, "--"));
#endif

	log("Analysing %s.%s for Xilinx DSP packing.\n", log_id(pm.module), log_id(st.dsp));

	Cell *cell = st.dsp;

	if (st.preAdd) {
		log("  preadder %s (%s)\n", log_id(st.preAdd), log_id(st.preAdd->type));
		bool A_SIGNED = st.preAdd->getParam(ID(A_SIGNED)).as_bool();
		bool D_SIGNED = st.preAdd->getParam(ID(B_SIGNED)).as_bool();
		if (st.sigA == st.preAdd->getPort(ID(B)))
			std::swap(A_SIGNED, D_SIGNED);
		st.sigA.extend_u0(30, A_SIGNED);
		st.sigD.extend_u0(25, D_SIGNED);
		cell->setPort(ID(A), st.sigA);
		cell->setPort(ID(D), st.sigD);
		cell->connections_.at(ID(INMODE)) = Const::from_string("00100");

		if (st.ffAD) {
			if (st.ffADcemux) {
				SigSpec S = st.ffADcemux->getPort(ID(S));
				cell->setPort(ID(CEAD), st.ffADcepol ? S : pm.module->Not(NEW_ID, S));
			}
			else
				cell->setPort(ID(CEAD), State::S1);
			cell->setParam(ID(ADREG), 1);
		}

		cell->setParam(ID(USE_DPORT), Const("TRUE"));

		pm.autoremove(st.preAdd);
	}
	if (st.postAdd) {
		log("  postadder %s (%s)\n", log_id(st.postAdd), log_id(st.postAdd->type));

		SigSpec &opmode = cell->connections_.at(ID(OPMODE));
		if (st.postAddMux) {
			log_assert(st.ffP);
			opmode[4] = st.postAddMux->getPort(ID(S));
			pm.autoremove(st.postAddMux);
		}
		else if (st.ffP && st.sigC == st.sigP)
			opmode[4] = State::S0;
		else
			opmode[4] = State::S1;
		opmode[6] = State::S0;
		opmode[5] = State::S1;

		if (opmode[4] != State::S0) {
			if (st.postAddMuxAB == ID(A))
				st.sigC.extend_u0(48, st.postAdd->getParam(ID(B_SIGNED)).as_bool());
			else
				st.sigC.extend_u0(48, st.postAdd->getParam(ID(A_SIGNED)).as_bool());
			cell->setPort(ID(C), st.sigC);
		}

		pm.autoremove(st.postAdd);
	}
	if (st.overflow) {
		log("  overflow %s (%s)\n", log_id(st.overflow), log_id(st.overflow->type));
		cell->setParam(ID(USE_PATTERN_DETECT), Const("PATDET"));
		cell->setParam(ID(SEL_PATTERN), Const("PATTERN"));
		cell->setParam(ID(SEL_MASK), Const("MASK"));

		if (st.overflow->type == ID($ge)) {
			Const B = st.overflow->getPort(ID(B)).as_const();
			log_assert(std::count(B.bits.begin(), B.bits.end(), State::S1) == 1);
			// Since B is an exact power of 2, subtract 1
			//   by inverting all bits up until hitting
			//   that one hi bit
			for (auto &b : B.bits)
				if (b == State::S0) b = State::S1;
				else if (b == State::S1) {
					b = State::S0;
					break;
				}
			B.extu(48);

			cell->setParam(ID(MASK), B);
			cell->setParam(ID(PATTERN), Const(0, 48));
			cell->setPort(ID(OVERFLOW), st.overflow->getPort(ID(Y)));
		}
		else log_abort();

		pm.autoremove(st.overflow);
	}

	if (st.clock != SigBit())
	{
		cell->setPort(ID(CLK), st.clock);

		auto f = [&pm,cell](SigSpec &A, Cell* ff, Cell* cemux, bool cepol, IdString ceport, Cell* rstmux, bool rstpol, IdString rstport) {
			SigSpec D = ff->getPort(ID(D));
			SigSpec Q = pm.sigmap(ff->getPort(ID(Q)));
			if (!A.empty())
				A.replace(Q, D);
			if (rstmux) {
				SigSpec Y = rstmux->getPort(ID(Y));
				SigSpec AB = rstmux->getPort(rstpol ? ID(A) : ID(B));
				if (!A.empty())
					A.replace(Y, AB);
				if (rstport != IdString()) {
					SigSpec S = rstmux->getPort(ID(S));
					cell->setPort(rstport, rstpol ? S : pm.module->Not(NEW_ID, S));
				}
			}
			else if (rstport != IdString())
				cell->setPort(rstport, State::S0);
			if (cemux) {
				SigSpec Y = cemux->getPort(ID(Y));
				SigSpec BA = cemux->getPort(cepol ? ID(B) : ID(A));
				SigSpec S = cemux->getPort(ID(S));
				if (!A.empty())
					A.replace(Y, BA);
				cell->setPort(ceport, cepol ? S : pm.module->Not(NEW_ID, S));
			}
			else
				cell->setPort(ceport, State::S1);

			for (auto c : Q.chunks()) {
				auto it = c.wire->attributes.find(ID(init));
				if (it == c.wire->attributes.end())
					continue;
				for (int i = c.offset; i < c.offset+c.width; i++) {
					log_assert(it->second[i] == State::S0 || it->second[i] == State::Sx);
					it->second[i] = State::Sx;
				}
			}
		};

		if (st.ffA2) {
			SigSpec &A = cell->connections_.at(ID(A));
			f(A, st.ffA2, st.ffA2cemux, st.ffA2cepol, ID(CEA2), st.ffA2rstmux, st.ffArstpol, ID(RSTA));
			pm.add_siguser(A, cell);
			if (st.ffA1) {
				f(A, st.ffA1, st.ffA1cemux, st.ffA1cepol, ID(CEA1), st.ffA1rstmux, st.ffArstpol, IdString());
				cell->setParam(ID(AREG), 2);
			}
			else
				cell->setParam(ID(AREG), 1);
		}
		if (st.ffB2) {
			SigSpec &B = cell->connections_.at(ID(B));
			f(B, st.ffB2, st.ffB2cemux, st.ffB2cepol, ID(CEB2), st.ffB2rstmux, st.ffBrstpol, ID(RSTB));
			pm.add_siguser(B, cell);
			if (st.ffB1) {
				f(B, st.ffB1, st.ffB1cemux, st.ffB1cepol, ID(CEB1), st.ffB1rstmux, st.ffBrstpol, IdString());
				cell->setParam(ID(BREG), 2);
			}
			else
				cell->setParam(ID(BREG), 1);
		}
		if (st.ffC) {
			SigSpec &C = cell->connections_.at(ID(C));
			f(C, st.ffC, st.ffCcemux, st.ffCcepol, ID(CEC), st.ffCrstmux, st.ffCrstpol, ID(RSTC));
			pm.add_siguser(C, cell);
			cell->setParam(ID(CREG), 1);
		}
		if (st.ffD) {
			SigSpec &D = cell->connections_.at(ID(D));
			f(D, st.ffD, st.ffDcemux, st.ffDcepol, ID(CED), st.ffDrstmux, st.ffDrstpol, ID(RSTD));
			pm.add_siguser(D, cell);
			cell->setParam(ID(DREG), 1);
		}
		if (st.ffM) {
			SigSpec M; // unused
			f(M, st.ffM, st.ffMcemux, st.ffMcepol, ID(CEM), st.ffMrstmux, st.ffMrstpol, ID(RSTM));
			st.ffM->connections_.at(ID(Q)).replace(st.sigM, pm.module->addWire(NEW_ID, GetSize(st.sigM)));
			cell->setParam(ID(MREG), State::S1);
		}
		if (st.ffP) {
			SigSpec P; // unused
			f(P, st.ffP, st.ffPcemux, st.ffPcepol, ID(CEP), st.ffPrstmux, st.ffPrstpol, ID(RSTP));
			st.ffP->connections_.at(ID(Q)).replace(st.sigP, pm.module->addWire(NEW_ID, GetSize(st.sigP)));
			cell->setParam(ID(PREG), State::S1);
		}

		log("  clock: %s (%s)", log_signal(st.clock), "posedge");

		if (st.ffA2) {
			log(" ffA2:%s", log_id(st.ffA2));
			if (st.ffA1)
				log(" ffA1:%s", log_id(st.ffA1));
		}

		if (st.ffAD)
			log(" ffAD:%s", log_id(st.ffAD));

		if (st.ffB2) {
			log(" ffB2:%s", log_id(st.ffB2));
			if (st.ffB1)
				log(" ffB1:%s", log_id(st.ffB1));
		}

		if (st.ffC)
			log(" ffC:%s", log_id(st.ffC));

		if (st.ffD)
			log(" ffD:%s", log_id(st.ffD));

		if (st.ffM)
			log(" ffM:%s", log_id(st.ffM));

		if (st.ffP)
			log(" ffP:%s", log_id(st.ffP));

		log("\n");
	}

	SigSpec P = st.sigP;
	if (GetSize(P) < 48)
		P.append(pm.module->addWire(NEW_ID, 48-GetSize(P)));
	cell->setPort(ID(P), P);

	pm.blacklist(cell);
}

struct XilinxDspPass : public Pass {
	XilinxDspPass() : Pass("xilinx_dsp", "Xilinx: pack resources into DSPs") { }
	void help() YS_OVERRIDE
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    xilinx_dsp [options] [selection]\n");
		log("\n");
		log("Pack input registers (A2, A1, B2, B1, C, D, AD; with optional enable/reset),\n");
		log("pipeline registers (M; with optional enable/reset), output registers (P; with\n");
		log("optional enable/reset), pre-adder and/or post-adder into Xilinx DSP resources.\n");
		log("\n");
		log("Multiply-accumulate operations using the post-adder with feedback on the 'C'\n");
		log("input will be folded into the DSP. In this scenario only, the 'C' input can be\n");
		log("used to override the current accumulation result with a new value, which will\n");
		log("be added to the multiplier result to form the next accumulation result.\n");
		log("\n");
		log("Use of the dedicated 'PCOUT' -> 'PCIN' cascade path is detected for 'P' -> 'C'\n");
		log("connections (optionally, where 'P' is right-shifted by 17-bits and used as an\n");
		log("input to the post-adder -- a pattern common for summing partial products to\n");
		log("implement wide multipliers).\n");
		log("\n");
		log("\n");
		log("Experimental feature: addition/subtractions less than 12 or 24 bits with the\n");
		log("'(* use_dsp=\"simd\" *)' attribute attached to the output wire or attached to\n");
		log("the add/subtract operator will cause those operations to be implemented using\n");
		log("the 'SIMD' feature of DSPs.\n");
		log("\n");
		log("Experimental feature: the presence of a `$ge' cell attached to the registered\n");
		log("P output implementing the operation \"(P >= <power-of-2>)\" will be transformed\n");
		log("into using the DSP48E1's pattern detector feature for overflow detection.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) YS_OVERRIDE
	{
		log_header(design, "Executing XILINX_DSP pass (pack resources into DSPs).\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			// if (args[argidx] == "-singleton") {
			// 	singleton_mode = true;
			// 	continue;
			// }
			break;
		}
		extra_args(args, argidx, design);

		for (auto module : design->selected_modules()) {
			pack_xilinx_simd(module, module->selected_cells());

			xilinx_dsp_pm pm(module, module->selected_cells());
			pm.run_xilinx_dsp_pack(pack_xilinx_dsp);

			do {
				did_something = false;
				xilinx_dsp_cascade_pm pmc(module, module->selected_cells());
				pmc.run_xilinx_dsp_cascadeP();
				pmc.run_xilinx_dsp_cascadeAB();
			} while (did_something);
		}
	}
} XilinxDspPass;

PRIVATE_NAMESPACE_END
