#ifndef __RISCV_META_H
#define __RISCV_META_H

enum rv_op
{
	rv_op_illegal = 0,
	rv_op_lui = 1,                     	/* Load Upper Immediate */
	rv_op_auipc = 2,                   	/* Add Upper Immediate to PC */
	rv_op_jal = 3,                     	/* Jump and Link */
	rv_op_jalr = 4,                    	/* Jump and Link Register */
	rv_op_beq = 5,                     	/* Branch Equal */
	rv_op_bne = 6,                     	/* Branch Not Equal */
	rv_op_blt = 7,                     	/* Branch Less Than */
	rv_op_bge = 8,                     	/* Branch Greater than Equal */
	rv_op_bltu = 9,                    	/* Branch Less Than Unsigned */
	rv_op_bgeu = 10,                   	/* Branch Greater than Equal Unsigned */
	rv_op_lb = 11,                     	/* Load Byte */
	rv_op_lh = 12,                     	/* Load Half */
	rv_op_lw = 13,                     	/* Load Word */
	rv_op_lbu = 14,                    	/* Load Byte Unsigned */
	rv_op_lhu = 15,                    	/* Load Half Unsigned */
	rv_op_sb = 16,                     	/* Store Byte */
	rv_op_sh = 17,                     	/* Store Half */
	rv_op_sw = 18,                     	/* Store Word */
	rv_op_addi = 19,                   	/* Add Immediate */
	rv_op_slti = 20,                   	/* Set Less Than Immediate */
	rv_op_sltiu = 21,                  	/* Set Less Than Immediate Unsigned */
	rv_op_xori = 22,                   	/* Xor Immediate */
	rv_op_ori = 23,                    	/* Or Immediate */
	rv_op_andi = 24,                   	/* And Immediate */
	rv_op_slli = 25,                   	/* Shift Left Logical Immediate */
	rv_op_srli = 26,                   	/* Shift Right Logical Immediate */
	rv_op_srai = 27,                   	/* Shift Right Arithmetic Immediate */
	rv_op_add = 28,                    	/* Add */
	rv_op_sub = 29,                    	/* Subtract */
	rv_op_sll = 30,                    	/* Shift Left Logical */
	rv_op_slt = 31,                    	/* Set Less Than */
	rv_op_sltu = 32,                   	/* Set Less Than Unsigned */
	rv_op_xor = 33,                    	/* Xor */
	rv_op_srl = 34,                    	/* Shift Right Logical */
	rv_op_sra = 35,                    	/* Shift Right Arithmetic */
	rv_op_or = 36,                     	/* Or */
	rv_op_and = 37,                    	/* And */
	rv_op_fence = 38,                  	/* Fence */
	rv_op_fence_i = 39,                	/* Fence Instruction */
	rv_op_lwu = 40,                    	/* Load Word Unsigned */
	rv_op_ld = 41,                     	/* Load Double */
	rv_op_sd = 42,                     	/* Store Double */
	rv_op_addiw = 43,                  	/* Add Immediate Word */
	rv_op_slliw = 44,                  	/* Shift Left Logical Immediate Word */
	rv_op_srliw = 45,                  	/* Shift Right Logical Immediate Word */
	rv_op_sraiw = 46,                  	/* Shift Right Arithmetic Immediate Word */
	rv_op_addw = 47,                   	/* Add Word */
	rv_op_subw = 48,                   	/* Subtract Word */
	rv_op_sllw = 49,                   	/* Shift Left Logical Word */
	rv_op_srlw = 50,                   	/* Shift Right Logical Word */
	rv_op_sraw = 51,                   	/* Shift Right Arithmetic Word */
	rv_op_ldu = 52,                    
	rv_op_lq = 53,                     
	rv_op_sq = 54,                     
	rv_op_addid = 55,                  
	rv_op_sllid = 56,                  
	rv_op_srlid = 57,                  
	rv_op_sraid = 58,                  
	rv_op_addd = 59,                   
	rv_op_subd = 60,                   
	rv_op_slld = 61,                   
	rv_op_srld = 62,                   
	rv_op_srad = 63,                   
	rv_op_mul = 64,                    	/* Multiply */
	rv_op_mulh = 65,                   	/* Multiply High Signed Signed */
	rv_op_mulhsu = 66,                 	/* Multiply High Signed Unsigned */
	rv_op_mulhu = 67,                  	/* Multiply High Unsigned Unsigned */
	rv_op_div = 68,                    	/* Divide Signed */
	rv_op_divu = 69,                   	/* Divide Unsigned */
	rv_op_rem = 70,                    	/* Remainder Signed */
	rv_op_remu = 71,                   	/* Remainder Unsigned */
	rv_op_mulw = 72,                   	/* Multiple Word */
	rv_op_divw = 73,                   	/* Divide Signed Word */
	rv_op_divuw = 74,                  	/* Divide Unsigned Word */
	rv_op_remw = 75,                   	/* Remainder Signed Word */
	rv_op_remuw = 76,                  	/* Remainder Unsigned Word */
	rv_op_muld = 77,                   
	rv_op_divd = 78,                   
	rv_op_divud = 79,                  
	rv_op_remd = 80,                   
	rv_op_remud = 81,                  
	rv_op_lr_w = 82,                   	/* Load Reserved Word */
	rv_op_sc_w = 83,                   	/* Store Conditional Word */
	rv_op_amoswap_w = 84,              	/* Atomic Swap Word */
	rv_op_amoadd_w = 85,               	/* Atomic Add Word */
	rv_op_amoxor_w = 86,               	/* Atomic Xor Word */
	rv_op_amoor_w = 87,                	/* Atomic Or Word */
	rv_op_amoand_w = 88,               	/* Atomic And Word */
	rv_op_amomin_w = 89,               	/* Atomic Minimum Word */
	rv_op_amomax_w = 90,               	/* Atomic Maximum Word */
	rv_op_amominu_w = 91,              	/* Atomic Minimum Unsigned Word */
	rv_op_amomaxu_w = 92,              	/* Atomic Maximum Unsigned Word */
	rv_op_lr_d = 93,                   	/* Load Reserved Double Word */
	rv_op_sc_d = 94,                   	/* Store Conditional Double Word */
	rv_op_amoswap_d = 95,              	/* Atomic Swap Double Word */
	rv_op_amoadd_d = 96,               	/* Atomic Add Double Word */
	rv_op_amoxor_d = 97,               	/* Atomic Xor Double Word */
	rv_op_amoor_d = 98,                	/* Atomic Or Double Word */
	rv_op_amoand_d = 99,               	/* Atomic And Double Word */
	rv_op_amomin_d = 100,              	/* Atomic Minimum Double Word */
	rv_op_amomax_d = 101,              	/* Atomic Maximum Double Word */
	rv_op_amominu_d = 102,             	/* Atomic Minimum Unsigned Double Word */
	rv_op_amomaxu_d = 103,             	/* Atomic Maximum Unsigned Double Word */
	rv_op_lr_q = 104,                  
	rv_op_sc_q = 105,                  
	rv_op_amoswap_q = 106,             
	rv_op_amoadd_q = 107,              
	rv_op_amoxor_q = 108,              
	rv_op_amoor_q = 109,               
	rv_op_amoand_q = 110,              
	rv_op_amomin_q = 111,              
	rv_op_amomax_q = 112,              
	rv_op_amominu_q = 113,             
	rv_op_amomaxu_q = 114,             
	rv_op_ecall = 115,                 	/* Environment Call */
	rv_op_ebreak = 116,                	/* Environment Break to Debugger */
	rv_op_uret = 117,                  	/* User Return */
	rv_op_sret = 118,                  	/* System Return */
	rv_op_hret = 119,                  	/* Hypervisor Return */
	rv_op_mret = 120,                  	/* Machine-Mode Return */
	rv_op_dret = 121,                  	/* Debug-Mode Return */
	rv_op_sfence_vm = 122,             	/* Supervisor Memory Management Fence */
	rv_op_wfi = 123,                   	/* Wait For Interrupt */
	rv_op_csrrw = 124,                 	/* CSR Atomic Read Write */
	rv_op_csrrs = 125,                 	/* CSR Atomic Set Bit */
	rv_op_csrrc = 126,                 	/* CSR Atomic Clear Bit */
	rv_op_csrrwi = 127,                	/* CSR Atomic Read Write Immediate */
	rv_op_csrrsi = 128,                	/* CSR Atomic Set Bit Immediate */
	rv_op_csrrci = 129,                	/* CSR Atomic Clear Bit Immediate */
	rv_op_flw = 130,                   	/* FP Load (SP) */
	rv_op_fsw = 131,                   	/* FP Store (SP) */
	rv_op_fmadd_s = 132,               	/* FP Fused Multiply Add (SP) */
	rv_op_fmsub_s = 133,               	/* FP Fused Multiply Subtract (SP) */
	rv_op_fnmsub_s = 134,              	/* FP Negate fused Multiply Subtract (SP) */
	rv_op_fnmadd_s = 135,              	/* FP Negate fused Multiply Add (SP) */
	rv_op_fadd_s = 136,                	/* FP Add (SP) */
	rv_op_fsub_s = 137,                	/* FP Subtract (SP) */
	rv_op_fmul_s = 138,                	/* FP Multiply (SP) */
	rv_op_fdiv_s = 139,                	/* FP Divide (SP) */
	rv_op_fsgnj_s = 140,               	/* FP Sign-injection (SP) */
	rv_op_fsgnjn_s = 141,              	/* FP Sign-injection Negate (SP) */
	rv_op_fsgnjx_s = 142,              	/* FP Sign-injection Xor (SP) */
	rv_op_fmin_s = 143,                	/* FP Minimum (SP) */
	rv_op_fmax_s = 144,                	/* FP Maximum (SP) */
	rv_op_fsqrt_s = 145,               	/* FP Square Root (SP) */
	rv_op_fle_s = 146,                 	/* FP Less Than Equal (SP) */
	rv_op_flt_s = 147,                 	/* FP Less Than (SP) */
	rv_op_feq_s = 148,                 	/* FP Equal (SP) */
	rv_op_fcvt_w_s = 149,              	/* FP Convert Float to Word (SP) */
	rv_op_fcvt_wu_s = 150,             	/* FP Convert Float to Word Unsigned (SP) */
	rv_op_fcvt_s_w = 151,              	/* FP Convert Word to Float (SP) */
	rv_op_fcvt_s_wu = 152,             	/* FP Convert Word Unsigned to Float (SP) */
	rv_op_fmv_x_s = 153,               	/* FP Move to Integer Register (SP) */
	rv_op_fclass_s = 154,              	/* FP Classify (SP) */
	rv_op_fmv_s_x = 155,               	/* FP Move from Integer Register (SP) */
	rv_op_fcvt_l_s = 156,              	/* FP Convert Float to Double Word (SP) */
	rv_op_fcvt_lu_s = 157,             	/* FP Convert Float to Double Word Unsigned (SP) */
	rv_op_fcvt_s_l = 158,              	/* FP Convert Double Word to Float (SP) */
	rv_op_fcvt_s_lu = 159,             	/* FP Convert Double Word Unsigned to Float (SP) */
	rv_op_fld = 160,                   	/* FP Load (DP) */
	rv_op_fsd = 161,                   	/* FP Store (DP) */
	rv_op_fmadd_d = 162,               	/* FP Fused Multiply Add (DP) */
	rv_op_fmsub_d = 163,               	/* FP Fused Multiply Subtract (DP) */
	rv_op_fnmsub_d = 164,              	/* FP Negate fused Multiply Subtract (DP) */
	rv_op_fnmadd_d = 165,              	/* FP Negate fused Multiply Add (DP) */
	rv_op_fadd_d = 166,                	/* FP Add (DP) */
	rv_op_fsub_d = 167,                	/* FP Subtract (DP) */
	rv_op_fmul_d = 168,                	/* FP Multiply (DP) */
	rv_op_fdiv_d = 169,                	/* FP Divide (DP) */
	rv_op_fsgnj_d = 170,               	/* FP to Sign-injection (DP) */
	rv_op_fsgnjn_d = 171,              	/* FP to Sign-injection Negate (DP) */
	rv_op_fsgnjx_d = 172,              	/* FP to Sign-injection Xor (DP) */
	rv_op_fmin_d = 173,                	/* FP Minimum (DP) */
	rv_op_fmax_d = 174,                	/* FP Maximum (DP) */
	rv_op_fcvt_s_d = 175,              	/* FP Convert DP to SP */
	rv_op_fcvt_d_s = 176,              	/* FP Convert SP to DP */
	rv_op_fsqrt_d = 177,               	/* Floating Square Root (DP) */
	rv_op_fle_d = 178,                 	/* FP Less Than Equal (DP) */
	rv_op_flt_d = 179,                 	/* FP Less Than (DP) */
	rv_op_feq_d = 180,                 	/* FP Equal (DP) */
	rv_op_fcvt_w_d = 181,              	/* FP Convert Float to Word (DP) */
	rv_op_fcvt_wu_d = 182,             	/* FP Convert Float to Word Unsigned (DP) */
	rv_op_fcvt_d_w = 183,              	/* FP Convert Word to Float (DP) */
	rv_op_fcvt_d_wu = 184,             	/* FP Convert Word Unsigned to Float (DP) */
	rv_op_fclass_d = 185,              	/* FP Classify (DP) */
	rv_op_fcvt_l_d = 186,              	/* FP Convert Float to Double Word (DP) */
	rv_op_fcvt_lu_d = 187,             	/* FP Convert Float to Double Word Unsigned (DP) */
	rv_op_fmv_x_d = 188,               	/* FP Move to Integer Register (DP) */
	rv_op_fcvt_d_l = 189,              	/* FP Convert Double Word to Float (DP) */
	rv_op_fcvt_d_lu = 190,             	/* FP Convert Double Word Unsigned Float (DP) */
	rv_op_fmv_d_x = 191,               	/* FP Move from Integer Register (DP) */
	rv_op_flq = 192,                   	/* FP Load (QP) */
	rv_op_fsq = 193,                   	/* FP Store (QP) */
	rv_op_fmadd_q = 194,               	/* FP Fused Multiply Add (QP) */
	rv_op_fmsub_q = 195,               	/* FP Fused Multiply Subtract (QP) */
	rv_op_fnmsub_q = 196,              	/* FP Negate fused Multiply Subtract (QP) */
	rv_op_fnmadd_q = 197,              	/* FP Negate fused Multiply Add (QP) */
	rv_op_fadd_q = 198,                	/* FP Add (QP) */
	rv_op_fsub_q = 199,                	/* FP Subtract (QP) */
	rv_op_fmul_q = 200,                	/* FP Multiply (QP) */
	rv_op_fdiv_q = 201,                	/* FP Divide (QP) */
	rv_op_fsgnj_q = 202,               	/* FP to Sign-injection (QP) */
	rv_op_fsgnjn_q = 203,              	/* FP to Sign-injection Negate (QP) */
	rv_op_fsgnjx_q = 204,              	/* FP to Sign-injection Xor (QP) */
	rv_op_fmin_q = 205,                	/* FP Minimum (QP) */
	rv_op_fmax_q = 206,                	/* FP Maximum (QP) */
	rv_op_fcvt_s_q = 207,              	/* FP Convert QP to SP */
	rv_op_fcvt_q_s = 208,              	/* FP Convert SP to QP */
	rv_op_fcvt_d_q = 209,              	/* FP Convert QP to DP */
	rv_op_fcvt_q_d = 210,              	/* FP Convert DP to QP */
	rv_op_fsqrt_q = 211,               	/* Floating Square Root (QP) */
	rv_op_fle_q = 212,                 	/* FP Less Than Equal (QP) */
	rv_op_flt_q = 213,                 	/* FP Less Than (QP) */
	rv_op_feq_q = 214,                 	/* FP Equal (QP) */
	rv_op_fcvt_w_q = 215,              	/* FP Convert Float to Word (QP) */
	rv_op_fcvt_wu_q = 216,             	/* FP Convert Float to Word Unsigned (QP) */
	rv_op_fcvt_q_w = 217,              	/* FP Convert Word to Float (QP) */
	rv_op_fcvt_q_wu = 218,             	/* FP Convert Word Unsigned to Float (QP) */
	rv_op_fclass_q = 219,              	/* FP Classify (QP) */
	rv_op_fcvt_l_q = 220,              	/* FP Convert Float to Double Word (QP) */
	rv_op_fcvt_lu_q = 221,             	/* FP Convert Float to Double Word Unsigned (QP) */
	rv_op_fcvt_q_l = 222,              	/* FP Convert Double Word to Float (QP) */
	rv_op_fcvt_q_lu = 223,             	/* FP Convert Double Word Unsigned Float (QP) */
	rv_op_fmv_x_q = 224,               	/* FP Move to Integer Register (QP) */
	rv_op_fmv_q_x = 225,               	/* FP Move from Integer Register (QP) */
	rv_op_c_addi4spn = 226,            
	rv_op_c_fld = 227,                 
	rv_op_c_lw = 228,                  
	rv_op_c_flw = 229,                 
	rv_op_c_fsd = 230,                 
	rv_op_c_sw = 231,                  
	rv_op_c_fsw = 232,                 
	rv_op_c_nop = 233,                 
	rv_op_c_addi = 234,                
	rv_op_c_jal = 235,                 
	rv_op_c_li = 236,                  
	rv_op_c_addi16sp = 237,            
	rv_op_c_lui = 238,                 
	rv_op_c_srli = 239,                
	rv_op_c_srai = 240,                
	rv_op_c_andi = 241,                
	rv_op_c_sub = 242,                 
	rv_op_c_xor = 243,                 
	rv_op_c_or = 244,                  
	rv_op_c_and = 245,                 
	rv_op_c_subw = 246,                
	rv_op_c_addw = 247,                
	rv_op_c_j = 248,                   
	rv_op_c_beqz = 249,                
	rv_op_c_bnez = 250,                
	rv_op_c_slli = 251,                
	rv_op_c_fldsp = 252,               
	rv_op_c_lwsp = 253,                
	rv_op_c_flwsp = 254,               
	rv_op_c_jr = 255,                  
	rv_op_c_mv = 256,                  
	rv_op_c_ebreak = 257,              
	rv_op_c_jalr = 258,                
	rv_op_c_add = 259,                 
	rv_op_c_fsdsp = 260,               
	rv_op_c_swsp = 261,                
	rv_op_c_fswsp = 262,               
	rv_op_c_ld = 263,                  
	rv_op_c_sd = 264,                  
	rv_op_c_addiw = 265,               
	rv_op_c_ldsp = 266,                
	rv_op_c_sdsp = 267,                
	rv_op_c_lq = 268,                  
	rv_op_c_sq = 269,                  
	rv_op_c_lqsp = 270,                
	rv_op_c_sqsp = 271,                
	rv_op_nop = 272,                   	/* No operation */
	rv_op_mv = 273,                    	/* Copy register */
	rv_op_not = 274,                   	/* One’s complement */
	rv_op_neg = 275,                   	/* Two’s complement */
	rv_op_negw = 276,                  	/* Two’s complement Word */
	rv_op_sext_w = 277,                	/* Sign extend Word */
	rv_op_seqz = 278,                  	/* Set if = zero */
	rv_op_snez = 279,                  	/* Set if ≠ zero */
	rv_op_sltz = 280,                  	/* Set if < zero */
	rv_op_sgtz = 281,                  	/* Set if > zero */
	rv_op_fmv_s = 282,                 	/* Single-precision move */
	rv_op_fabs_s = 283,                	/* Single-precision absolute value */
	rv_op_fneg_s = 284,                	/* Single-precision negate */
	rv_op_fmv_d = 285,                 	/* Double-precision move */
	rv_op_fabs_d = 286,                	/* Double-precision absolute value */
	rv_op_fneg_d = 287,                	/* Double-precision negate */
	rv_op_fmv_q = 288,                 	/* Quadruple-precision move */
	rv_op_fabs_q = 289,                	/* Quadruple-precision absolute value */
	rv_op_fneg_q = 290,                	/* Quadruple-precision negate */
	rv_op_beqz = 291,                  	/* Branch if = zero */
	rv_op_bnez = 292,                  	/* Branch if ≠ zero */
	rv_op_blez = 293,                  	/* Branch if ≤ zero */
	rv_op_bgez = 294,                  	/* Branch if ≥ zero */
	rv_op_bltz = 295,                  	/* Branch if < zero */
	rv_op_bgtz = 296,                  	/* Branch if > zero */
	rv_op_ble = 297,                   
	rv_op_bleu = 298,                  
	rv_op_bgt = 299,                   
	rv_op_bgtu = 300,                  
	rv_op_j = 301,                     	/* Jump */
	rv_op_ret = 302,                   	/* Return from subroutine */
	rv_op_jr = 303,                    	/* Jump register */
	rv_op_rdcycle = 304,               	/* Read Cycle Counter Status Register */
	rv_op_rdtime = 305,                	/* Read Timer Status register */
	rv_op_rdinstret = 306,             	/* Read Instructions Retired Status Register */
	rv_op_rdcycleh = 307,              	/* Read Cycle Counter Status Register (upper 32-bits on RV32) */
	rv_op_rdtimeh = 308,               	/* Read Timer Status register (upper 32-bits on RV32) */
	rv_op_rdinstreth = 309,            	/* Read Instructions Retired Status Register (upper 32-bits on RV32) */
	rv_op_frcsr = 310,                 	/* Read FP Control and Status Register */
	rv_op_frrm = 311,                  	/* Read FP Rounding Mode */
	rv_op_frflags = 312,               	/* Read FP Accrued Exception Flags */
	rv_op_fscsr = 313,                 	/* Set FP Control and Status Register */
	rv_op_fsrm = 314,                  	/* Set FP Rounding Mode */
	rv_op_fsflags = 315,               	/* Set FP Accrued Exception Flags */
	rv_op_fsrmi = 316,                 	/* Set FP Rounding Mode Immediate */
	rv_op_fsflagsi = 317,              	/* Set FP Accrued Exception Flags Immediate */
	rv_op_last = 318,              
};

struct riscvinstr {
      unsigned int opcode;
      bool has_alu;
      bool has_mul;
	  bool has_div;
      bool has_fpu;
      bool has_fdiv;
      bool has_ifpu;
      bool is_memory;     
};

const riscvinstr instrlist[] = {
    // opcode, has_alu, has_mul, has_div, has_fpu, has_fdiv, has_ifpu, is_memory
	{ rv_op_illegal,  0, 0, 0, 0, 0, 0, 0 },  //    0              
    { rv_op_lui,      1, 0, 0, 0, 0, 0, 0 },  //    1        RV32I alu
	{ rv_op_auipc,    1, 0, 0, 0, 0, 0, 0 },  //    2        RV32I alu
    { rv_op_jal,      1, 0, 0, 0, 0, 0, 0 },  //    3        RV32I jump
    { rv_op_jalr,     1, 0, 0, 0, 0, 0, 0 },  //    4        RV32I jump indirect
    { rv_op_beq,      1, 0, 0, 0, 0, 0, 0 },  //    5        RV32I branch
	{ rv_op_bne,      1, 0, 0, 0, 0, 0, 0 },  //    6        RV32I branch
	{ rv_op_blt,      1, 0, 0, 0, 0, 0, 0 },  //    7        RV32I branch
	{ rv_op_bge,      1, 0, 0, 0, 0, 0, 0 },  //    8        RV32I branch
	{ rv_op_bltu,     1, 0, 0, 0, 0, 0, 0 },  //    9        RV32I branch
	{ rv_op_bgeu,     1, 0, 0, 0, 0, 0, 0 },  //    10       RV32I branch
    { rv_op_lb,       1, 0, 0, 0, 0, 0, 1 },  //    11       RV32I load
	{ rv_op_lh,       1, 0, 0, 0, 0, 0, 1 },  //    12       RV32I load
	{ rv_op_lw,       1, 0, 0, 0, 0, 0, 1 },  //    13       RV32I load
	{ rv_op_lbu,      1, 0, 0, 0, 0, 0, 1 },  //    14       RV32I load
	{ rv_op_lhu,      1, 0, 0, 0, 0, 0, 1 },  //    15       RV32I load	
    { rv_op_sb,       1, 0, 0, 0, 0, 0, 1 },  //    16       RV32I store
	{ rv_op_sh,       1, 0, 0, 0, 0, 0, 1 },  //    17       RV32I store
	{ rv_op_sw,       1, 0, 0, 0, 0, 0, 1 },  //    18       RV32I store              	
	{ rv_op_addi,     1, 0, 0, 0, 0, 0, 0 },  //    19       RV32I alu              	
	{ rv_op_slti,     1, 0, 0, 0, 0, 0, 0 },  //    20       RV32I alu             
	{ rv_op_sltiu,    1, 0, 0, 0, 0, 0, 0 },  //    21       RV32I alu             	
	{ rv_op_xori,     1, 0, 0, 0, 0, 0, 0 },  //    22       RV32I alu             	
	{ rv_op_ori,      1, 0, 0, 0, 0, 0, 0 },  //    23       RV32I alu               	
	{ rv_op_andi,     1, 0, 0, 0, 0, 0, 0 },  //    24       RV32I alu             	
	{ rv_op_slli,     1, 0, 0, 0, 0, 0, 0 },  //    25       RV32I alu               	
	{ rv_op_srli,     1, 0, 0, 0, 0, 0, 0 },  //    26       RV32I alu               	
	{ rv_op_srai,     1, 0, 0, 0, 0, 0, 0 },  //    27       RV32I alu              	
	{ rv_op_add,      1, 0, 0, 0, 0, 0, 0 },  //    28       RV32I alu             	
	{ rv_op_sub,      1, 0, 0, 0, 0, 0, 0 },  //    29       RV32I alu              	
	{ rv_op_sll,      1, 0, 0, 0, 0, 0, 0 },  //    30       RV32I alu             	
	{ rv_op_slt,      1, 0, 0, 0, 0, 0, 0 },  //    31       RV32I alu             	
	{ rv_op_sltu,     1, 0, 0, 0, 0, 0, 0 },  //    32       RV32I alu            	
	{ rv_op_xor,      1, 0, 0, 0, 0, 0, 0 },  //    33       RV32I alu             	
	{ rv_op_srl,      1, 0, 0, 0, 0, 0, 0 },  //    34       RV32I alu              
	{ rv_op_sra,      1, 0, 0, 0, 0, 0, 0 },  //    35       RV32I alu
	{ rv_op_or,       1, 0, 0, 0, 0, 0, 0 },  //    36       RV32I alu
	{ rv_op_and,      1, 0, 0, 0, 0, 0, 0 },  //    37       RV32I alu    
    { rv_op_fence,    1, 0, 0, 0, 0, 0, 0 },  //    38       RV32I fence
	{ rv_op_fence_i,  1, 0, 0, 0, 0, 0, 0 },  //    39       RV32I fence
    { rv_op_lwu,      1, 0, 0, 0, 0, 0, 1 },  //    40       RV32I load

    { rv_op_ld,       1, 0, 0, 0, 0, 0, 1 },  //    41       RV64I load
    { rv_op_sd,       1, 0, 0, 0, 0, 0, 1 },  //    42       RV64I store
    { rv_op_addiw,    1, 0, 0, 0, 0, 0, 0 },  //    43       RV64I alu
	{ rv_op_slliw,    1, 0, 0, 0, 0, 0, 0 },  //    44       RV64I alu
	{ rv_op_srliw,    1, 0, 0, 0, 0, 0, 0 },  //    45       RV64I alu
	{ rv_op_sraiw,    1, 0, 0, 0, 0, 0, 0 },  //    46       RV64I alu
	{ rv_op_addw,     1, 0, 0, 0, 0, 0, 0 },  //    47       RV64I alu
	{ rv_op_subw,     1, 0, 0, 0, 0, 0, 0 },  //    48       RV64I alu
	{ rv_op_sllw,     1, 0, 0, 0, 0, 0, 0 },  //    49       RV64I alu
	{ rv_op_srlw,     1, 0, 0, 0, 0, 0, 0 },  //    50       RV64I alu
	{ rv_op_sraw,     1, 0, 0, 0, 0, 0, 0 },  //    51       RV64I alu

	{ rv_op_ldu,      1, 0, 0, 0, 0, 0, 0 },  //    52                    
	{ rv_op_lq,       1, 0, 0, 0, 0, 0, 0 },  //    53                     
	{ rv_op_sq,       1, 0, 0, 0, 0, 0, 0 },  //    54                     
	{ rv_op_addid,    1, 0, 0, 0, 0, 0, 0 },  //    55                  
	{ rv_op_sllid,    1, 0, 0, 0, 0, 0, 0 },  //    56                  
	{ rv_op_srlid,    1, 0, 0, 0, 0, 0, 0 },  //    57                  
	{ rv_op_sraid,    1, 0, 0, 0, 0, 0, 0 },  //    58                  
	{ rv_op_addd,     1, 0, 0, 0, 0, 0, 0 },  //    59                   
	{ rv_op_subd,     1, 0, 0, 0, 0, 0, 0 },  //    60                   
	{ rv_op_slld,     1, 0, 0, 0, 0, 0, 0 },  //    61                   
	{ rv_op_srld,     1, 0, 0, 0, 0, 0, 0 },  //    62                   
	{ rv_op_srad,     1, 0, 0, 0, 0, 0, 0 },  //    63                   

    { rv_op_mul,      1, 1, 0, 0, 0, 0, 0 },  //    64       RV32M alu multiply
	{ rv_op_mulh,     1, 1, 0, 0, 0, 0, 0 },  //    65       RV32M alu multiply
	{ rv_op_mulhsu,   1, 1, 0, 0, 0, 0, 0 },  //    66       RV32M alu multiply
	{ rv_op_mulhu,    1, 1, 0, 0, 0, 0, 0 },  //    67       RV32M alu multiply
    { rv_op_div,      1, 0, 1, 0, 0, 0, 0 },  //    68       RV32M alu divide
	{ rv_op_divu,     1, 0, 1, 0, 0, 0, 0 },  //    69       RV32M alu divide
	{ rv_op_rem,      1, 0, 1, 0, 0, 0, 0 },  //    70       RV32M alu divide
	{ rv_op_remu,     1, 0, 1, 0, 0, 0, 0 },  //    71       RV32M alu divide

    { rv_op_mulw,     1, 1, 0, 0, 0, 0, 0 },  //    72       RV64M alu multiply	
	{ rv_op_divw,     1, 0, 1, 0, 0, 0, 0 },  //    73       RV64M alu divide
	{ rv_op_divuw,    1, 0, 1, 0, 0, 0, 0 },  //    74       RV64M alu divide
	{ rv_op_remw,     1, 0, 1, 0, 0, 0, 0 },  //    75       RV64M alu divide
	{ rv_op_remuw,    1, 0, 1, 0, 0, 0, 0 },  //    76       RV64M alu divide
	
	{ rv_op_muld,     1, 1, 0, 0, 0, 0, 0 },  //    77                   
	{ rv_op_divd,     1, 0, 1, 0, 0, 0, 0 },  //    78                   
	{ rv_op_divud,    1, 0, 1, 0, 0, 0, 0 },  //    79                  
	{ rv_op_remd,     1, 0, 1, 0, 0, 0, 0 },  //    80                   
	{ rv_op_remud,    1, 0, 1, 0, 0, 0, 0 },  //    81                  
      
    { rv_op_lr_w,           1, 0, 0, 0, 0, 0, 1 },  //    82    RV32A atomic
	{ rv_op_sc_w,           1, 0, 0, 0, 0, 0, 1 },  //    83    RV32A atomic
	{ rv_op_amoswap_w,      1, 0, 0, 0, 0, 0, 1 },  //    84    RV32A atomic
	{ rv_op_amoadd_w,       1, 0, 0, 0, 0, 0, 0 },  //    85    RV32A atomic
	{ rv_op_amoxor_w,       1, 0, 0, 0, 0, 0, 0 },  //    86    RV32A atomic
	{ rv_op_amoor_w,        1, 0, 0, 0, 0, 0, 0 },  //    87    RV32A atomic
	{ rv_op_amoand_w,       1, 0, 0, 0, 0, 0, 0 },  //    88    RV32A atomic
	{ rv_op_amomin_w,       1, 0, 0, 0, 0, 0, 0 },  //    89    RV32A atomic
	{ rv_op_amomax_w,       1, 0, 0, 0, 0, 0, 0 },  //    90    RV32A atomic
	{ rv_op_amominu_w,      1, 0, 0, 0, 0, 0, 0 },  //    91    RV32A atomic
	{ rv_op_amomaxu_w,      1, 0, 0, 0, 0, 0, 0 },  //    92    RV32A atomic
	
	{ rv_op_lr_d,           1, 0, 0, 0, 0, 0, 1 },  //    93    RV64A atomic
	{ rv_op_sc_d,           1, 0, 0, 0, 0, 0, 1 },  //    94    RV64A atomic
	{ rv_op_amoswap_d,      1, 0, 0, 0, 0, 0, 1 },  //    95    RV64A atomic
	{ rv_op_amoadd_d,       1, 0, 0, 0, 0, 0, 0 },  //    96    RV64A atomic
	{ rv_op_amoxor_d,       1, 0, 0, 0, 0, 0, 0 },  //    97    RV64A atomic
	{ rv_op_amoor_d,        1, 0, 0, 0, 0, 0, 0 },  //    98    RV64A atomic
	{ rv_op_amoand_d,       1, 0, 0, 0, 0, 0, 0 },  //    99    RV64A atomic
	{ rv_op_amomin_d,       1, 0, 0, 0, 0, 0, 0 },  //    100   RV64A atomic
	{ rv_op_amomax_d,       1, 0, 0, 0, 0, 0, 0 },  //    101   RV64A atomic
	{ rv_op_amominu_d,      1, 0, 0, 0, 0, 0, 0 },  //    102   RV64A atomic
	{ rv_op_amomaxu_d,      1, 0, 0, 0, 0, 0, 0 },  //    103   RV64A atomic
	
	{ rv_op_lr_q,           1, 0, 0, 0, 0, 0, 1 },  //    104                  
	{ rv_op_sc_q,           1, 0, 0, 0, 0, 0, 1 },  //    105                  
	{ rv_op_amoswap_q,      1, 0, 0, 0, 0, 0, 1 },  //    106             
	{ rv_op_amoadd_q,       1, 0, 0, 0, 0, 0, 0 },  //    107              
	{ rv_op_amoxor_q,       1, 0, 0, 0, 0, 0, 0 },  //    108              
	{ rv_op_amoor_q,        1, 0, 0, 0, 0, 0, 0 },  //    109               
	{ rv_op_amoand_q,       1, 0, 0, 0, 0, 0, 0 },  //    110              
	{ rv_op_amomin_q,       1, 0, 0, 0, 0, 0, 0 },  //    111              
	{ rv_op_amomax_q,       1, 0, 0, 0, 0, 0, 0 },  //    112              
	{ rv_op_amominu_q,      1, 0, 0, 0, 0, 0, 0 },  //    113             
	{ rv_op_amomaxu_q,      1, 0, 0, 0, 0, 0, 0 },  //    114             

    { rv_op_ecall,          1, 0, 0, 0, 0, 0, 0 },  //    115      RV32S system
	{ rv_op_ebreak,         1, 0, 0, 0, 0, 0, 0 },  //    116      RV32S system
	{ rv_op_uret,           1, 0, 0, 0, 0, 0, 0 },  //    117      RV32S system
	{ rv_op_sret,           1, 0, 0, 0, 0, 0, 0 },  //    118      RV32S system
	{ rv_op_hret,           1, 0, 0, 0, 0, 0, 0 },  //    119      RV32S system
	{ rv_op_mret,           1, 0, 0, 0, 0, 0, 0 },  //    120      RV32S system
	{ rv_op_dret,           1, 0, 0, 0, 0, 0, 0 },  //    121      RV32S system
	{ rv_op_sfence_vm,      1, 0, 0, 0, 0, 0, 0 },  //    122      RV32S system
	{ rv_op_wfi,            1, 0, 0, 0, 0, 0, 0 },  //    123      RV32S system

    { rv_op_csrrw,          1, 0, 0, 0, 0, 0, 0 },  //    124      RV32S csr
	{ rv_op_csrrs,          1, 0, 0, 0, 0, 0, 0 },  //    125      RV32S csr
	{ rv_op_csrrc,          1, 0, 0, 0, 0, 0, 0 },  //    126      RV32S csr
	{ rv_op_csrrwi,         1, 0, 0, 0, 0, 0, 0 },  //    127      RV32S csr
	{ rv_op_csrrsi,         1, 0, 0, 0, 0, 0, 0 },  //    128      RV32S csr
	{ rv_op_csrrci,         1, 0, 0, 0, 0, 0, 0 },  //    129      RV32S csr
	
    { rv_op_flw,            1, 0, 0, 1, 0, 0, 1 },  //    130      RV32F fpu load
    { rv_op_fsw,            1, 0, 0, 1, 0, 0, 1 },  //    131      RV32F fpu store
    { rv_op_fmadd_s,        1, 0, 0, 1, 0, 0, 0 },  //    132      RV32F fpu fma
	{ rv_op_fmsub_s,        1, 0, 0, 1, 0, 0, 0 },  //    133      RV32F fpu fma
	{ rv_op_fnmsub_s,       1, 0, 0, 1, 0, 0, 0 },  //    134      RV32F fpu fma
	{ rv_op_fnmadd_s,       1, 0, 0, 1, 0, 0, 0 },  //    135      RV32F fpu fma

	{ rv_op_fadd_s,         1, 0, 0, 1, 0, 0, 0 },  //    136      RV32F fpu
	{ rv_op_fsub_s,         1, 0, 0, 1, 0, 0, 0 },  //    137      RV32F fpu
	{ rv_op_fmul_s,         1, 0, 0, 1, 1, 0, 0 },  //    138      RV32F fpu
    { rv_op_fdiv_s,         1, 0, 0, 1, 1, 0, 0 },  //    139      RV32F fpu fdiv
	{ rv_op_fsgnj_s,        1, 0, 0, 1, 0, 0, 0 },  //    140      RV32F fpu
	{ rv_op_fsgnjn_s,       1, 0, 0, 1, 0, 0, 0 },  //    141      RV32F fpu
	{ rv_op_fsgnjx_s,       1, 0, 0, 1, 0, 0, 0 },  //    142      RV32F fpu
	{ rv_op_fmin_s,         1, 0, 0, 1, 0, 0, 0 },  //    143      RV32F fpu
	{ rv_op_fmax_s,         1, 0, 0, 1, 0, 0, 0 },  //    144      RV32F fpu
    { rv_op_fsqrt_s,        1, 0, 0, 1, 0, 0, 0 },  //    145      RV32F fpu fsqrt
	{ rv_op_fle_s,          1, 0, 0, 1, 0, 0, 0 },  //    146      RV32F fpu
	{ rv_op_flt_s,          1, 0, 0, 1, 0, 0, 0 },  //    147      RV32F fpu
	{ rv_op_feq_s,          1, 0, 0, 1, 0, 0, 0 },  //    148      RV32F fpu
	{ rv_op_fcvt_w_s,       1, 0, 0, 1, 0, 1, 0 },  //    149      RV32F fpu fcvt          	
	{ rv_op_fcvt_wu_s,      1, 0, 0, 1, 0, 1, 0 },  //    150      RV32F fpu fcvt
	{ rv_op_fcvt_s_w,       1, 0, 0, 1, 0, 1, 0 },  //    151      RV32F fpu fcvt
	{ rv_op_fcvt_s_wu,      1, 0, 0, 1, 0, 1, 0 },  //    152      RV32F fpu fcvt

    { rv_op_fmv_x_s,        1, 0, 0, 1, 0, 0, 0 },  //    153      RV32F fpu fmove	
	{ rv_op_fclass_s,       1, 0, 0, 1, 0, 0, 0 },  //    154      RV32F fpu
    { rv_op_fmv_s_x,        1, 0, 0, 1, 0, 0, 0 },  //    155      RV32F fpu fmove	
	
    { rv_op_fcvt_l_s,       1, 0, 0, 1, 0, 1, 0 },  //    156      RV64F fpu fcvt
	{ rv_op_fcvt_lu_s,      1, 0, 0, 1, 0, 1, 0 },  //    157      RV64F fpu fcvt
	{ rv_op_fcvt_s_l,       1, 0, 0, 1, 0, 1, 0 },  //    158      RV64F fpu fcvt
	{ rv_op_fcvt_s_lu,      1, 0, 0, 1, 0, 1, 0 },  //    159      RV64F fpu fcvt

    { rv_op_fld,            1, 0, 0, 1, 0, 0, 1 },  //    160      RV32D fpu load
    { rv_op_fsd,            1, 0, 0, 1, 0, 0, 1 },  //    161      RV32D fpu store

	{ rv_op_fmadd_d,        1, 0, 0, 1, 0, 0, 0 },  //    162      RV32D fpu fma
	{ rv_op_fmsub_d,        1, 0, 0, 1, 0, 0, 0 },  //    163      RV32D fpu fma
	{ rv_op_fnmsub_d,       1, 0, 0, 1, 0, 0, 0 },  //    164      RV32D fpu fma
	{ rv_op_fnmadd_d,       1, 0, 0, 1, 0, 0, 0 },  //    165      RV32D fpu fma

	{ rv_op_fadd_d,         1, 0, 0, 1, 0, 0, 0 },  //    166      RV32D fpu
	{ rv_op_fsub_d,         1, 0, 0, 1, 0, 0, 0 },  //    167      RV32D fpu
	{ rv_op_fmul_d,         1, 0, 0, 1, 1, 0, 0 },  //    168      RV32D fpu
    { rv_op_fdiv_d,         1, 0, 0, 1, 1, 0, 0 },  //    169      RV32D fpu fdiv
	{ rv_op_fsgnj_d,        1, 0, 0, 1, 0, 0, 0 },  //    170      RV32D fpu
	{ rv_op_fsgnjn_d,       1, 0, 0, 1, 0, 0, 0 },  //    171      RV32D fpu
	{ rv_op_fsgnjx_d,       1, 0, 0, 1, 0, 0, 0 },  //    172      RV32D fpu
	{ rv_op_fmin_d,         1, 0, 0, 1, 0, 0, 0 },  //    173      RV32D fpu
	{ rv_op_fmax_d,         1, 0, 0, 1, 0, 0, 0 },  //    174      RV32D fpu
    { rv_op_fcvt_s_d,       1, 0, 0, 1, 0, 1, 0 },  //    175      RV32D fpu fcvt
	{ rv_op_fcvt_d_s,       1, 0, 0, 1, 0, 1, 0 },  //    176      RV32D fpu fcvt
    { rv_op_fsqrt_d,        1, 0, 0, 1, 0, 0, 0 },  //    177      RV32D fpu fsqrt
	{ rv_op_fle_d,          1, 0, 0, 1, 0, 0, 0 },  //    178      RV32D fpu
	{ rv_op_flt_d,          1, 0, 0, 1, 0, 0, 0 },  //    179      RV32D fpu
	{ rv_op_feq_d,          1, 0, 0, 1, 0, 0, 0 },  //    180      RV32D fpu
    { rv_op_fcvt_w_d,       1, 0, 0, 1, 0, 1, 0 },  //    181      RV32D fpu fcvt
	{ rv_op_fcvt_wu_d,      1, 0, 0, 1, 0, 1, 0 },  //    182      RV32D fpu fcvt
	{ rv_op_fcvt_d_w,       1, 0, 0, 1, 0, 1, 0 },  //    183      RV32D fpu fcvt
	{ rv_op_fcvt_d_wu,      1, 0, 0, 1, 0, 1, 0 },  //    184      RV32D fpu fcvt
	{ rv_op_fclass_d,       1, 0, 0, 1, 0, 0, 0 },  //    185      RV32D fpu

	{ rv_op_fcvt_l_d,       1, 0, 0, 1, 0, 1, 0 },  //    186      RV64D fpu fcvt
	{ rv_op_fcvt_lu_d,      1, 0, 0, 1, 0, 1, 0 },  //    187      RV64D fpu fcvt
    { rv_op_fmv_x_d,        1, 0, 0, 1, 0, 0, 0 },  //    188      RV64D fpu fmove
	{ rv_op_fcvt_d_l,       1, 0, 0, 1, 0, 1, 0 },  //    189      RV64D fpu fcvt
	{ rv_op_fcvt_d_lu,      1, 0, 0, 1, 0, 1, 0 },  //    190      RV64D fpu fcvt    
	{ rv_op_fmv_d_x,        1, 0, 0, 1, 0, 0, 0 },  //    191      RV64D fpu fmove

	{ rv_op_flq,            1, 0, 0, 1, 0, 0, 1 },  //    192                   	
	{ rv_op_fsq,            1, 0, 0, 1, 0, 0, 1 },  //    193                   	
	{ rv_op_fmadd_q,        1, 0, 0, 1, 0, 0, 0 },  //    194               	
	{ rv_op_fmsub_q,        1, 0, 0, 1, 0, 0, 0 },  //    195               	
	{ rv_op_fnmsub_q,       1, 0, 0, 1, 0, 0, 0 },  //    196              	
	{ rv_op_fnmadd_q,       1, 0, 0, 1, 0, 0, 0 },  //    197              	
	{ rv_op_fadd_q,         1, 0, 0, 1, 0, 0, 0 },  //    198                	
	{ rv_op_fsub_q,         1, 0, 0, 1, 0, 0, 0 },  //    199                
	{ rv_op_fmul_q,         1, 0, 0, 1, 1, 0, 0 },  //    200                
	{ rv_op_fdiv_q,         1, 0, 0, 1, 1, 0, 0 },  //    201                
	{ rv_op_fsgnj_q,        1, 0, 0, 1, 0, 0, 0 },  //    202           
	{ rv_op_fsgnjn_q,       1, 0, 0, 1, 0, 0, 0 },  //    203       
	{ rv_op_fsgnjx_q,       1, 0, 0, 1, 0, 0, 0 },  //    204              	
	{ rv_op_fmin_q,         1, 0, 0, 1, 0, 0, 0 },  //    205             
	{ rv_op_fmax_q,         1, 0, 0, 1, 0, 0, 0 },  //    206                
	{ rv_op_fcvt_s_q,       1, 0, 0, 1, 0, 0, 0 },  //    207              	
	{ rv_op_fcvt_q_s,       1, 0, 0, 1, 0, 0, 0 },  //    208              	
	{ rv_op_fcvt_d_q,       1, 0, 0, 1, 0, 0, 0 },  //    209
	{ rv_op_fcvt_q_d,       1, 0, 0, 1, 0, 0, 0 },  //    210
	{ rv_op_fsqrt_q,        1, 0, 0, 1, 0, 0, 0 },  //    211
	{ rv_op_fle_q,          1, 0, 0, 1, 0, 0, 0 },  //    212
	{ rv_op_flt_q,          1, 0, 0, 1, 0, 0, 0 },  //    213
	{ rv_op_feq_q,          1, 0, 0, 1, 0, 0, 0 },  //    214
	{ rv_op_fcvt_w_q,       1, 0, 0, 1, 0, 0, 0 },  //    215
	{ rv_op_fcvt_wu_q,      1, 0, 0, 1, 0, 0, 0 },  //    216
	{ rv_op_fcvt_q_w,       1, 0, 0, 1, 0, 0, 0 },  //    217
	{ rv_op_fcvt_q_wu,      1, 0, 0, 1, 0, 0, 0 },  //    218
	{ rv_op_fclass_q,       1, 0, 0, 1, 0, 0, 0 },  //    219
	{ rv_op_fcvt_l_q,       1, 0, 0, 1, 0, 0, 0 },  //    220
	{ rv_op_fcvt_lu_q,      1, 0, 0, 1, 0, 0, 0 },  //    221
	{ rv_op_fcvt_q_l,       1, 0, 0, 1, 0, 0, 0 },  //    222
	{ rv_op_fcvt_q_lu,      1, 0, 0, 1, 0, 0, 0 },  //    223
	{ rv_op_fmv_x_q,        1, 0, 0, 1, 0, 0, 0 },  //    224
	{ rv_op_fmv_q_x,        1, 0, 0, 1, 0, 0, 0 },  //    225

	{ rv_op_c_addi4spn,     1, 0, 0, 0, 0, 0, 0 },  //    226            
	{ rv_op_c_fld,          1, 0, 0, 0, 0, 0, 1 },  //    227                 
	{ rv_op_c_lw,           1, 0, 0, 0, 0, 0, 1 },  //    228                  
	{ rv_op_c_flw,          1, 0, 0, 0, 0, 0, 1 },  //    229                 
	{ rv_op_c_fsd,          1, 0, 0, 0, 0, 0, 1 },  //    230                 
	{ rv_op_c_sw,           1, 0, 0, 0, 0, 0, 1 },  //    231                  
	{ rv_op_c_fsw,          1, 0, 0, 0, 0, 0, 1 },  //    232                 
	{ rv_op_c_nop,          1, 0, 0, 0, 0, 0, 0 },  //    233                 
	{ rv_op_c_addi,         1, 0, 0, 0, 0, 0, 0 },  //    234                
	{ rv_op_c_jal,          1, 0, 0, 0, 0, 0, 0 },  //    235                 
	{ rv_op_c_li,           1, 0, 0, 0, 0, 0, 0 },  //    236                  
	{ rv_op_c_addi16sp,     1, 0, 0, 0, 0, 0, 0 },  //    237            
	{ rv_op_c_lui,          1, 0, 0, 0, 0, 0, 0 },  //    238                 
	{ rv_op_c_srli,         1, 0, 0, 0, 0, 0, 0 },  //    239                
	{ rv_op_c_srai,         1, 0, 0, 0, 0, 0, 0 },  //    240                
	{ rv_op_c_andi,         1, 0, 0, 0, 0, 0, 0 },  //    241                
	{ rv_op_c_sub,          1, 0, 0, 0, 0, 0, 0 },  //    242                 
	{ rv_op_c_xor,          1, 0, 0, 0, 0, 0, 0 },  //    243                 
	{ rv_op_c_or,           1, 0, 0, 0, 0, 0, 0 },  //    244                  
	{ rv_op_c_and,          1, 0, 0, 0, 0, 0, 0 },  //    245                 
	{ rv_op_c_subw,         1, 0, 0, 0, 0, 0, 0 },  //    246                
	{ rv_op_c_addw,         1, 0, 0, 0, 0, 0, 0 },  //    247                
	{ rv_op_c_j,            1, 0, 0, 0, 0, 0, 0 },  //    248                   
	{ rv_op_c_beqz,         1, 0, 0, 0, 0, 0, 0 },  //    249                
	{ rv_op_c_bnez,         1, 0, 0, 0, 0, 0, 0 },  //    250                
	{ rv_op_c_slli,         1, 0, 0, 0, 0, 0, 0 },  //    251                
	{ rv_op_c_fldsp,        1, 0, 0, 0, 0, 0, 0 },  //    252               
	{ rv_op_c_lwsp,         1, 0, 0, 0, 0, 0, 0 },  //    253                
	{ rv_op_c_flwsp,        1, 0, 0, 0, 0, 0, 0 },  //    254               
	{ rv_op_c_jr,           1, 0, 0, 0, 0, 0, 0 },  //    255                  
	{ rv_op_c_mv,           1, 0, 0, 0, 0, 0, 0 },  //    256                  
	{ rv_op_c_ebreak,       1, 0, 0, 0, 0, 0, 0 },  //    257              
	{ rv_op_c_jalr,         1, 0, 0, 0, 0, 0, 0 },  //    258
	{ rv_op_c_add,          1, 0, 0, 0, 0, 0, 0 },  //    259
	{ rv_op_c_fsdsp,        1, 0, 0, 0, 0, 0, 0 },  //    260
	{ rv_op_c_swsp,         1, 0, 0, 0, 0, 0, 0 },  //    261
	{ rv_op_c_fswsp,        1, 0, 0, 0, 0, 0, 0 },  //    262
	{ rv_op_c_ld,           1, 0, 0, 0, 0, 0, 1 },  //    263
	{ rv_op_c_sd,           1, 0, 0, 0, 0, 0, 1 },  //    264
	{ rv_op_c_addiw,        1, 0, 0, 0, 0, 0, 0 },  //    265
	{ rv_op_c_ldsp,         1, 0, 0, 0, 0, 0, 1 },  //    266
	{ rv_op_c_sdsp,         1, 0, 0, 0, 0, 0, 1 },  //    267
	{ rv_op_c_lq,           1, 0, 0, 0, 0, 0, 1 },  //    268
	{ rv_op_c_sq,           1, 0, 0, 0, 0, 0, 1 },  //    269
	{ rv_op_c_lqsp,         1, 0, 0, 0, 0, 0, 0 },  //    270
	{ rv_op_c_sqsp,         1, 0, 0, 0, 0, 0, 0 },  //    271
	{ rv_op_nop,            1, 0, 0, 0, 0, 0, 0 },  //    272
	{ rv_op_mv,             1, 0, 0, 0, 0, 0, 0 },  //    273
	{ rv_op_not,            1, 0, 0, 0, 0, 0, 0 },  //    274
	{ rv_op_neg,            1, 0, 0, 0, 0, 0, 0 },  //    275
	{ rv_op_negw,           1, 0, 0, 0, 0, 0, 0 },  //    276
	{ rv_op_sext_w,         1, 0, 0, 0, 0, 0, 0 },  //    277
	{ rv_op_seqz,           1, 0, 0, 0, 0, 0, 0 },  //    278
	{ rv_op_snez,           1, 0, 0, 0, 0, 0, 0 },  //    279
	{ rv_op_sltz,           1, 0, 0, 0, 0, 0, 0 },  //    280
	{ rv_op_sgtz,           1, 0, 0, 0, 0, 0, 0 },  //    281
	{ rv_op_fmv_s,          1, 0, 0, 0, 0, 0, 0 },  //    282
	{ rv_op_fabs_s,         1, 0, 0, 0, 0, 0, 0 },  //    283
	{ rv_op_fneg_s,         1, 0, 0, 0, 0, 0, 0 },  //    284
	{ rv_op_fmv_d,          1, 0, 0, 0, 0, 0, 0 },  //    285
	{ rv_op_fabs_d,         1, 0, 0, 0, 0, 0, 0 },  //    286
	{ rv_op_fneg_d,         1, 0, 0, 0, 0, 0, 0 },  //    287
	{ rv_op_fmv_q,          1, 0, 0, 0, 0, 0, 0 },  //    288
	{ rv_op_fabs_q,         1, 0, 0, 0, 0, 0, 0 },  //    289
	{ rv_op_fneg_q,         1, 0, 0, 0, 0, 0, 0 },  //    290
	{ rv_op_beqz,           1, 0, 0, 0, 0, 0, 0 },  //    291
	{ rv_op_bnez,           1, 0, 0, 0, 0, 0, 0 },  //    292
	{ rv_op_blez,           1, 0, 0, 0, 0, 0, 0 },  //    293
	{ rv_op_bgez,           1, 0, 0, 0, 0, 0, 0 },  //    294
	{ rv_op_bltz,           1, 0, 0, 0, 0, 0, 0 },  //    295
	{ rv_op_bgtz,           1, 0, 0, 0, 0, 0, 0 },  //    296
	{ rv_op_ble,            1, 0, 0, 0, 0, 0, 0 },  //    297
	{ rv_op_bleu,           1, 0, 0, 0, 0, 0, 0 },  //    298
	{ rv_op_bgt,            1, 0, 0, 0, 0, 0, 0 },  //    299
	{ rv_op_bgtu,           1, 0, 0, 0, 0, 0, 0 },  //    300
	{ rv_op_j,              1, 0, 0, 0, 0, 0, 0 },  //    301
	{ rv_op_ret,            1, 0, 0, 0, 0, 0, 0 },  //    302
	{ rv_op_jr,             1, 0, 0, 0, 0, 0, 0 },  //    303 

	{ rv_op_rdcycle,        1, 0, 0, 0, 0, 0, 0 },  //    304      RV32S csr
	{ rv_op_rdtime,         1, 0, 0, 0, 0, 0, 0 },  //    305      RV32S csr
	{ rv_op_rdinstret,      1, 0, 0, 0, 0, 0, 0 },  //    306      RV32S csr
	{ rv_op_rdcycleh,       1, 0, 0, 0, 0, 0, 0 },  //    307      RV32S csr
	{ rv_op_rdtimeh,        1, 0, 0, 0, 0, 0, 0 },  //    308      RV32S csr
	{ rv_op_rdinstreth,     1, 0, 0, 0, 0, 0, 0 },  //    309      RV32S csr

    { rv_op_frcsr,          1, 0, 0, 0, 0, 0, 0 },  //    310      RV32FD csr
	{ rv_op_frrm,           1, 0, 0, 0, 0, 0, 0 },  //    311      RV32FD csr
	{ rv_op_frflags,        1, 0, 0, 0, 0, 0, 0 },  //    312      RV32FD csr
	{ rv_op_fscsr,          1, 0, 0, 0, 0, 0, 0 },  //    313      RV32FD csr
	{ rv_op_fsrm,           1, 0, 0, 0, 0, 0, 0 },  //    314      RV32FD csr
	{ rv_op_fsflags,        1, 0, 0, 0, 0, 0, 0 },  //    315      RV32FD csr
	{ rv_op_fsrmi,          1, 0, 0, 0, 0, 0, 0 },  //    316      RV32FD csr
	{ rv_op_fsflagsi,       1, 0, 0, 0, 0, 0, 0 },  //    317      RV32FD csr
    { rv_op_last,           0, 0, 0, 0, 0, 0, 0 }
};


#endif // __RISCV_META_H