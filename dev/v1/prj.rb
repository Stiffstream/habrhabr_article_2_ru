#!/usr/bin/ruby
require 'rubygems'

gem 'Mxx_ru', '>= 1.3.0'

require 'mxx_ru/cpp'

MxxRu::Cpp::exe_target {

  target 'v1_app'

  required_prj 'so_5/prj.rb'

  cpp_source 'main.cpp'
}

