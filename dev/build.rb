#!/usr/bin/ruby
require 'mxx_ru/cpp'

MxxRu::Cpp::composite_target( MxxRu::BUILD_ROOT ) {

	toolset.force_cpp14
	global_include_path "."

	if 'gcc' == toolset.name || 'clang' == toolset.name
		global_linker_option '-pthread'
		global_linker_option '-static-libstdc++'
		global_linker_option "-Wl,-rpath='$ORIGIN'"
	end

	# If there is local options file then use it.
	if FileTest.exist?( "local-build.rb" )
		required_prj "local-build.rb"
	else
		default_runtime_mode( MxxRu::Cpp::RUNTIME_RELEASE )
		MxxRu::enable_show_brief
		global_obj_placement MxxRu::Cpp::RuntimeSubdirObjPlacement.new( 'target' )
	end

	required_prj 'v1/prj.rb'
	required_prj 'v2/prj.rb'
	required_prj 'v3/prj.rb'
	required_prj 'v4/prj.rb'
}
