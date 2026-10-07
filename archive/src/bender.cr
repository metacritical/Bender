require "readline"
require "./cli-parser"

module Bender::Main
  extend self

  def version
    puts "Bender #{VERSION}"
  end

  def print_banner
    print Banner
  end

  def run(code)
    puts "Run file #{code}"
  end

  def repl
    version
    print_banner

    loop {
      input = Readline.readline("user=> ")
      exit(0) if (input == "exit")
      run input
    }
  end
end
