 ###############################################################
 # 
 # Copyright 2011 Red Hat, Inc. 
 # 
 # Licensed under the Apache License, Version 2.0 (the "License"); you 
 # may not use this file except in compliance with the License.  You may 
 # obtain a copy of the License at 
 # 
 #    http://www.apache.org/licenses/LICENSE-2.0 
 # 
 # Unless required by applicable law or agreed to in writing, software 
 # distributed under the License is distributed on an "AS IS" BASIS, 
 # WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 # See the License for the specific language governing permissions and 
 # limitations under the License. 
 # 
 ############################################################### 

MACRO (CONDOR_UNIT_TEST _CNDR_TARGET _SRCS _LINK_LIBS )

	if (BUILD_TESTING)

		enable_testing()

		# we are dependent on boost unit testing framework.
		include_directories(${BOOST_INCLUDE})
		add_definitions(-DBOOST_TEST_DYN_LINK)

		set ( LOCAL_${_CNDR_TARGET} ${_CNDR_TARGET} )

		if ( WINDOWS )
			string (REPLACE ".exe" "" ${LOCAL_${_CNDR_TARGET}} ${LOCAL_${_CNDR_TARGET}})
		endif( WINDOWS )

		add_executable( ${LOCAL_${_CNDR_TARGET}} ${_SRCS})
		
		if ( WINDOWS )
			set_property( TARGET ${LOCAL_${_CNDR_TARGET}} PROPERTY FOLDER "tests" )
		endif ( WINDOWS )

		condor_set_link_libs( ${LOCAL_${_CNDR_TARGET}} "${_LINK_LIBS}" )

		add_test ( ${LOCAL_${_CNDR_TARGET}}_unit_test
			   ${LOCAL_${_CNDR_TARGET}} )

		APPEND_VAR( CONDOR_TESTS ${_CNDR_TARGET} )

	endif(BUILD_TESTING)

ENDMACRO(CONDOR_UNIT_TEST)
