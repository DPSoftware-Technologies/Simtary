# libs/JSBSimC - JSBSim (static, from the sparse clone in deps/jsbsim) and SimtaryJSBSim, the
# shared library with a plain C interface (jsbsim_c.h) that the game links instead.
#
# Its own directory on purpose: the workspace's top-level add_compile_definitions
# (_HAS_EXCEPTIONS=0) and /EHsc- reach every target declared in that directory. JSBSim throws,
# and its exceptions are caught in jsbsim_c.cpp - both have to be built with exceptions on,
# and with the same standard library configuration.

# The file list is upstream's own (src/*/CMakeLists.txt), written out rather
# than globbed, because the checkout carries sources upstream does NOT build:
# the CLI simulator (JSBSim.cpp, JSBSim.minimal.cpp), the retired simplex-trim
# and Nelder-Mead experiments, FGMars, the MSIS self-test, and the expat
# fragments xmltok_impl.c / xmltok_ns.c that xmltok.c #includes. A GLOB would
# compile all of them, and then fail to link on the duplicate main().
add_library(jsbsim STATIC
    ${SIMTARY_JSBSIM_SRC}/FGFDMExec.cpp
    ${SIMTARY_JSBSIM_SRC}/FGJSBBase.cpp

    ${SIMTARY_JSBSIM_SRC}/initialization/FGInitialCondition.cpp
    ${SIMTARY_JSBSIM_SRC}/initialization/FGTrim.cpp
    ${SIMTARY_JSBSIM_SRC}/initialization/FGTrimAxis.cpp
    ${SIMTARY_JSBSIM_SRC}/initialization/FGLinearization.cpp

    ${SIMTARY_JSBSIM_SRC}/input_output/FGGroundCallback.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGPropertyManager.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGScript.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGXMLElement.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGXMLParse.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGfdmSocket.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGXMLFileRead.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGOutputType.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGOutputFG.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGOutputSocket.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGOutputFile.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGOutputTextFile.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGPropertyReader.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGModelLoader.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGInputType.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGInputSocket.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/FGUDPInputSocket.cpp
    ${SIMTARY_JSBSIM_SRC}/input_output/string_utilities.cpp

    ${SIMTARY_JSBSIM_SRC}/math/FGColumnVector3.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGFunction.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGLocation.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGMatrix33.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGPropertyValue.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGQuaternion.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGRealValue.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGTable.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGCondition.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGRungeKutta.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGModelFunctions.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGTemplateFunc.cpp
    ${SIMTARY_JSBSIM_SRC}/math/FGStateSpace.cpp

    ${SIMTARY_JSBSIM_SRC}/models/FGAerodynamics.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGAircraft.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGAtmosphere.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGAuxiliary.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGFCS.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGSurface.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGGroundReactions.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGInertial.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGLGear.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGMassBalance.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGModel.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGOutput.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGPropagate.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGPropulsion.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGInput.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGExternalReactions.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGExternalForce.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGBuoyantForces.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGGasCell.cpp
    ${SIMTARY_JSBSIM_SRC}/models/FGAccelerations.cpp

    ${SIMTARY_JSBSIM_SRC}/models/atmosphere/FGMSIS.cpp
    ${SIMTARY_JSBSIM_SRC}/models/atmosphere/FGStandardAtmosphere.cpp
    ${SIMTARY_JSBSIM_SRC}/models/atmosphere/FGWinds.cpp
    ${SIMTARY_JSBSIM_SRC}/models/atmosphere/MSIS/nrlmsise-00.c
    ${SIMTARY_JSBSIM_SRC}/models/atmosphere/MSIS/nrlmsise-00_data.c

    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGDeadBand.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGFCSComponent.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGFilter.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGGain.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGKinemat.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGSummer.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGSwitch.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGFCSFunction.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGSensor.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGPID.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGActuator.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGAccelerometer.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGGyro.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGMagnetometer.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGAngles.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGWaypoint.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGDistributor.cpp
    ${SIMTARY_JSBSIM_SRC}/models/flight_control/FGLinearActuator.cpp

    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGElectric.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGEngine.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGForce.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGNozzle.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGPiston.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGPropeller.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGRocket.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGTank.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGThruster.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGTurbine.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGTurboProp.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGTransmission.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGRotor.cpp
    ${SIMTARY_JSBSIM_SRC}/models/propulsion/FGBrushLessDCMotor.cpp

    ${SIMTARY_JSBSIM_SRC}/simgear/props/props.cxx
    ${SIMTARY_JSBSIM_SRC}/simgear/magvar/coremag.cxx
    ${SIMTARY_JSBSIM_SRC}/simgear/misc/sg_path.cxx
    ${SIMTARY_JSBSIM_SRC}/simgear/misc/strutils.cxx
    ${SIMTARY_JSBSIM_SRC}/simgear/io/iostreams/sgstream.cxx
    ${SIMTARY_JSBSIM_SRC}/simgear/xml/easyxml.cxx
    ${SIMTARY_JSBSIM_SRC}/simgear/xml/xmlparse.c
    ${SIMTARY_JSBSIM_SRC}/simgear/xml/xmltok.c
    ${SIMTARY_JSBSIM_SRC}/simgear/xml/xmlrole.c

    ${SIMTARY_JSBSIM_SRC}/GeographicLib/Geodesic.cpp
    ${SIMTARY_JSBSIM_SRC}/GeographicLib/GeodesicLine.cpp
    ${SIMTARY_JSBSIM_SRC}/GeographicLib/Math.cpp
)

target_include_directories(jsbsim PUBLIC ${SIMTARY_JSBSIM_SRC})
set_target_properties(jsbsim PROPERTIES
    OUTPUT_NAME "jsbsim_static"  # jsbsim.lib is the DLL's import library
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    C_STANDARD 11
    FOLDER "Simtary/ThirdParty"
)
# JSBSIM_STATIC_LINK, _USE_MATH_DEFINES and NOMINMAX are upstream's own PUBLIC
# definitions - JSBSim_API.h reads the first to stop declaring dllimport, and
# the headers need the other two on MSVC. XML_STATIC is for the expat copy
# bundled in simgear/xml.
target_compile_definitions(jsbsim
    PUBLIC  JSBSIM_STATIC_LINK
    PRIVATE JSBSIM_VERSION="${SIMTARY_JSBSIM_VERSION}"
            XML_STATIC
            $<$<BOOL:${MINGW}>:_WIN32_WINNT=0x600>
)
if (MSVC)
    target_compile_definitions(jsbsim PUBLIC _USE_MATH_DEFINES NOMINMAX)
    # Exceptions and RTTI ON for this target whatever the directory scope says,
    # and warnings off: it is third-party and its diagnostics would bury the
    # engine's, the same call SimtaryModelIO makes.
    target_compile_options(jsbsim PRIVATE /EHsc /GR /w)
else()
    target_compile_options(jsbsim PRIVATE -fexceptions -frtti -w)
endif()
if (WIN32)
    target_link_libraries(jsbsim PUBLIC wsock32 ws2_32)  # FGfdmSocket
elseif (UNIX)
    target_link_libraries(jsbsim PUBLIC m)
endif()

# The C interface: one shared library holding JSBSim and the shim (LGPL 2.1 as a whole).
add_library(SimtaryJSBSim SHARED jsbsim_c.cpp jsbsim_c.h)
target_link_libraries(SimtaryJSBSim PRIVATE jsbsim)
target_include_directories(SimtaryJSBSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(SimtaryJSBSim INTERFACE SIMTARY_HAS_JSBSIM=1)
set_target_properties(SimtaryJSBSim PROPERTIES
    OUTPUT_NAME "jsbsim"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Simtary/ThirdParty"
)
if (MSVC)
    target_compile_options(SimtaryJSBSim PRIVATE /EHsc /GR)
else()
    target_compile_options(SimtaryJSBSim PRIVATE -fexceptions -frtti)
    set_target_properties(jsbsim PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
