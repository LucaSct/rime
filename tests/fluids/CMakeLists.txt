rime_add_test(rime_fluids_tests buoyancy_test.cpp basin_test.cpp phase_change_test.cpp
                              thermal_contact_test.cpp thermal_field_test.cpp)
target_link_libraries(rime_fluids_tests PRIVATE rime::fluids rime::physics)
