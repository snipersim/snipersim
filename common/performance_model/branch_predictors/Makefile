
all: ibtb simple_bimodal_table_test global_predictor_test

#global_predictor_test: ../../../lib/libcarbon_sim.a global_predictor_test.C global_predictor.h
#	g++44 -lpthread -I../../../boost_1_38_0/include -I. -I../../system -I../../config -I../../misc -I../../performance_model ../../system/simulator.o ../../../lib/libcarbon_sim.a global_predictor_test.C -o global_predictor_test

ibtb: ../../../lib/libcarbon_sim.a ibtb.C ibtb.h
	g++44 -lpthread -I../../../boost_1_38_0/include -I. -I../../system -I../../config -I../../misc -I../../performance_model ../../system/simulator.o ../../../lib/libcarbon_sim.a ibtb.C -o ibtb

simple_bimodal_table_test: ../../../lib/libcarbon_sim.a simple_bimodal_table_test.C saturating_predictor.h
	g++44 -lpthread -I ../../../boost_1_38_0/include/ -I. -I../../system -I../../config -I../../misc -I../../performance_model ../../system/simulator.o ../../../lib/libcarbon_sim.a simple_bimodal_table_test.C -o simple_bimodal_table_test

global_predictor_test: ../../../lib/libcarbon_sim.a global_predictor_test.C global_predictor.h
	g++44 ./global_predictor_test.C -o global_predictor_test ./branch_predictor_return_value.cc -I ../../../common/misc -I../../../boost_1_38_0/include -I. -I../../system -I../../config -I../../misc -I../../performance_m -I. -I../ ../../../lib/libcarbon_sim.a  -lpthread

