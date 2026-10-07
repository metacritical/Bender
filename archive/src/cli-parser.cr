require "option_parser"
require "./banner"

OptionParser.parse do |parser|
  _SYS_INFO = ""

  {% if flag?(:win32) %}
    _SYS_INFO = "Windows"
  {% elsif flag?(:linux) %}
    _SYS_INFO = "Linux"
  {% elsif flag?(:darwin) %}
    _SYS_INFO = "Darwin"
  {% else %}
    _SYS_INFO = "Generic OS"
  {% end %}

  
  parser.on "-v", "--version", "Show version" do
    puts "Bender's Clojure #{Bender::VERSION} [#{_SYS_INFO}]"
    exit
  end

  parser.on "-h", "--help", "Show help" do
    parser.banner = Bender::Banner
    puts parser
    exit
  end

  parser.invalid_option do |flag|
    STDERR.puts "ERROR: #{flag} is not a valid option."
    STDERR.puts parser
    exit(1)
  end

  parser.unknown_args do |args, _|
    if args.empty?
      Bender::Main.repl
    else
      file = ARGV[0]
      code = File.read(file) if File.exists?(file)
      Bender::Main.run(code)
    end
  end
end
