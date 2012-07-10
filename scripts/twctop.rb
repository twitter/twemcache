#!/usr/bin/env ruby

require "optparse"
require "socket"
require "yaml"
require "benchmark"

DELAY = 0.05
NA = " -- "
SLAB_SIZE = 1024 * 1024
EXPIRY = 14 * 24 * 60 * 60

# We have three views: slab, command and host
# host view should be the summary of the cache pool
# slab view tells the distribution of items among slab classes
# command view summarizes the mix, hit/miss/success rate of commands

# FIXME: split slab view into two: slab-data view and slab-command view
# In the slab-command view we should use the bar to show stacked r/w/d requests

SLAB_FIELDS = { # minimum width: 115 characters; 140+ to make the bar look good
  :size         => { :title => "SLAB", :width => 7, :justify => :left }, # item size b
  :slab         => { :title => "#SLAB", :width => 7 },           # number of slabs
  :item         => { :title => "ITEM", :width => 11 },           # absolute number of items
  :item_pct     => { :title => "ITEM(%)", :width => 11 },        # slab utilization
  :data_pct     => { :title => "KEYVAL(%)", :width => 11 },      # effective use of items
  :payload_pct  => { :title => "VALUE(%)", :width => 11 },       # value/(key+value) ratio
  :expire       => { :title => "REC(/s)", :width => 8 },
  :evict        => { :title => "EVT(/s)", :width => 8 },
  :slab_evict   => { :title => "SEVT(/s)", :width => 9 },       # slab eviction
  :locate       => { :title => "LOCATE(/s)", :width => 11 },
  :insert       => { :title => "INSERT(/s)", :width => 11 },
  :bar          => { :title => " ", :width => 0 },              # mem util/allocation bar
}

HOST_FIELDS = { # minimum width: 140 characters, if this is too wide remove the REQ fields
  :host     => { :title => "INSTANCE", :width => 24, :justify => :left }, # ip:port
  :uptime   => { :title => "UPTIME", :width => 7 },
  :util     => { :title => "UTIL(%)", :width => 10 },    # data / max memory allowed
  :conn     => { :title => "CONN", :width => 10 },
  :latency  => { :title => "LATENCY", :width => 8 },        # rt time for "stats"
  :expire   => { :title => "EXPIRE(/s)", :width => 11},
  :evict    => { :title => "EVICT(/s)", :width => 11 },
  :rdata    => { :title => "BYTE_IN/s", :width => 11 },
  :wdata    => { :title => "BYTE_OUT/s", :width => 11 },
  :req      => { :title => "REQ(/s)", :width => 11 },
  :svrerr   => { :title => "SVRERR(/s)", :width => 11 },
}

CMD_FIELDS = { # minimum width: 92 characters
  :command  => { :title => "COMMAND", :width => 8, :justify => :left },
  :rate     => { :title => "REQUEST(/s)", :width => 12 },
  :success  => { :title => "SUCCESS(%)", :width => 12},
  :hit      => { :title => "HIT(%)", :width => 12 },
  :miss     => { :title => "MISS(%)", :width => 12 },
  :exist    => { :title => "EXISTS(%)", :width => 12 },
  :error    => { :title => "CMDERR(/s)", :width => 12 },
  :mix      => { :title => "MIX(%)", :width => 12 }, # % in total commands
  #  : => { :title => "(%)", :width =>  },
}

CMD = [
  :set, :cas, :add, :replace, :append, :prepend, :incr, :decr, # write
  :get, :gets,                                                 # read
  :delete,                                                     # delete
]
CMD_WRITE = [
  :set, :cas, :add, :replace, :append, :prepend, :incr, :decr,
]
CMD_READ = [
  :get, :gets,
]
CMD_SUCCESS = [
  :set, :cas, :add, :replace, :append, :prepend, :incr, :decr,
]
CMD_HITMISS = [
  :cas, :replace, :append, :prepend, :incr, :decr, :get, :gets, :delete,
]
CMD_EXIST = [
  :cas, :add,
]


# change me to alter the displayed columns or their order
SLAB_ORDER = [
              :size, :slab,               # slab basics
              :item, :item_pct, :data_pct, :payload_pct,
                                          # memory
              :bar,                       # allocated memory
              :expire, :evict,            # item
              :slab_evict,                # slab
              :locate, :insert,           # command
             ]
HOST_ORDER = [
              :host,                      # name
	          :uptime,		              # uptime
              :util, :conn,               # usage
              :latency,                   # responsiveness
              :expire, :evict,            # churning
              :rdata, :wdata,             # throughput
              :req,                       # command counts
              :svrerr,                    # server errors
             ]
CMD_ORDER = [
             :command,                   # name
             :rate,                      # rate sent
             :mix,                       # command mix
             :success, :hit,             # success update/locate
             :miss, :exist, :error,      # errors or failures
            ]

# display buffer
class ViewBuffer
  attr_accessor :headers, :rows, :top_row

  def initialize
    @headers = []
    @rows = []
    @top_row = 0
  end
end

$buf = ViewBuffer.new
$winch = false


# format number/time to fit their column width
class Fixnum
  def to_h
    if self < 1.1 * 1024
      "%7d" % self
    elsif self < 1.1 * 1024 ** 2
      "%6.1fK" % (self / 1024.0)
    elsif self < 1.1 * 1024 ** 3
      "%6.1fM" % (self / (1024.0 ** 2))
    elsif self < 1.1 * 1024 ** 4
      "%6.1fG" % (self / (1024.0 ** 3))
    else
      "%6.1fT" % (self / (1024.0 ** 4))
    end
  end

  def to_time_string
    if self <= 90
      "%4ds" % self
    elsif self <= 90 * 60
      "%4dm" % (self / 60)
    else
      # 4 digits allow us to show an age of 9999 hrs/400 days, should be enough
      # we decide not to go beyond hours coz age in days isn't very informative
      "%4dh" % (self / 3600)
    end
  end
end

class Float
  def to_time_string
    if self <= 0.0015
      "%4dus" % (self * 1_000_000).to_i
    elsif self <= 1.5
      "%4dms" % (self * 1000).to_i
    elsif self <= 90.0
      "%3dsec" % self
    elsif self <= 90 * 60
      "%3dmin" % self
    else
      "%4dhr" % self
    end
  end
end

class String
  def text_size
    self.gsub(/\033\[[\d;]*./, "").size
  end

  def colorize(color)
    if !$options[:raw]
      "\033[#{color}m#{self}\033[39m"
    else
      self
    end
  end

  def red; self.colorize(31); end
  def green; self.colorize(32); end
  def blue; self.colorize(34); end
  def cyan; self.colorize(36); end
  def reverse; "\033[7m#{self}\033[27m"; end
end

class ServerStats
  attr_accessor :host, :slabs, :stats, :cmds

  def initialize(host)
    @host = host
    @slabs = {}  # { chunk_size => { stat => value } }
    @stats = {}  # { stat => value }
    @cmds = {
      :count    => {},
      :error    => {},
      :success  => {},
      :hit      => {},
      :miss     => {},
      :exist    => {},
    }
    name, port = host.split(":")
    stats_data = {}
    # global stats + timing
    mc = TCPSocket.new(name, port)
    elapsed = Benchmark.realtime do
      mc.write("stats\r\n")
      while (line = mc.gets) !~ /^END/
        if line =~ /^STAT (\S+) (\S+)/
          stats_data[$1] = $2
        end
      end
    end
    stats_setting = {}
    mc.write("stats settings\r\n")
    while (line = mc.gets) !~ /^END/
      if line =~ /^STAT (\S+) (\S+)/
        stats_setting[$1] = $2
      end
    end

    # slab stats
    mc.write("stats slabs\r\n")
    slab_data = []
    while (line = mc.gets) !~ /^END/
      if line =~ /^STAT (\d+):(\S+) (-?\d+)/
        while slab_data.size <= $1.to_i
          slab_data << {}
        end
        slab_data[$1.to_i][$2] = $3.to_i
      end
    end

    # commands related metrics
    CMD.each do |cmd|
      @cmds[:count][cmd] = stats_data[cmd.id2name].to_i
      @cmds[:error][cmd] = stats_data[cmd.id2name + "_error"].to_i
    end
    CMD_SUCCESS.each do |cmd|
      @cmds[:success][cmd] = stats_data[cmd.id2name + "_success"].to_i
    end
    CMD_HITMISS.each do |cmd|
      @cmds[:hit][cmd] = stats_data[cmd.id2name + "_hit"].to_i
      @cmds[:miss][cmd] = stats_data[cmd.id2name + "_miss"].to_i
    end
    @cmds[:exist][:cas] = stats_data["cas_badval"].to_i
    @cmds[:exist][:add] = stats_data["add_exist"].to_i
    @cmds[:timestamp] = stats_data["aggregate_ts"].to_f

    @stats = {
      :uptime         => stats_data["uptime"].to_i,
      :latency        => elapsed,
      :data           => stats_data["data_curr"].to_i,
      :maxbytes       => stats_setting["maxbytes"].to_i,
      :conn           => stats_data["conn_curr"].to_i,
      :req            => CMD.inject(0) { |n, c| n += @cmds[:count][c] },
      :expire         => stats_data["item_expire"].to_i,
      :evict          => stats_data["item_evict"].to_i,
      :rdata          => stats_data["data_read"].to_i,
      :wdata          => stats_data["data_written"].to_i,
      :svrerr         => stats_data["server_error"].to_i,
      :timestamp      => stats_data["aggregate_ts"].to_f,
    }
    slab_data.each do |d|
      next if d.empty?
      @slabs[d["chunk_size"]] = {
        :data_curr    => d["data_curr"],
        :data_value   => d["data_value_curr"],
        :item_curr    => d["item_curr"],
        :slab_curr    => d["slab_curr"],
        :evict        => d["item_evict"],
        :slab_evict   => d["slab_evict"],
        :expire       => d["item_expire"],
        :locate       => CMD_HITMISS.inject(0) { |n, c| n += d[c.id2name + "_hit"] },
        :insert       => CMD_SUCCESS.inject(0) { |n, c| n += d[c.id2name + "_success"] },
      } if d["chunk_size"]
    end
  rescue => e
    # ignore
  end
end

class SummaryStats
  attr_accessor :slabs, :hosts, :cmds

  def initialize(host_stats_arr)
    @slabs = {}  # { chunk_size => { stat => value } }
    @hosts = {}  # { host => {stat => value} }
    @cmds = {
      :count    => {},
      :error    => {},
      :success  => {},
      :hit      => {},
      :miss     => {},
      :exist    => {},
    }

    @cmds[:timestamp] = 0.0
    host_stats_arr.each do |host_stats|

      # copy the per-host global metrics
      @hosts[host_stats.host] = host_stats.stats

      # get per-slab metrics by sum up each across all hosts
      host_stats.slabs.each do |chunk_size, slab_stats|
        if @slabs[chunk_size]
          slab_stats.each do |k, v|
            @slabs[chunk_size][k] += v
          end
        else
          slabs[chunk_size] = slab_stats
        end
      end

      # sum up command metrics across all hosts
      CMD.each do |cmd|
        for category in [:count, :error, :success, :hit, :miss, :exist]
          if host_stats.cmds[category][cmd]
            if @cmds[category][cmd]
              @cmds[category][cmd] += host_stats.cmds[category][cmd]
            else
              @cmds[category][cmd] = host_stats.cmds[category][cmd]
            end
          end
        end
      end
      @cmds[:timestamp] += host_stats.cmds[:timestamp]
    end
    #FIXME: getting timestamp this way doesn't really make sense
    @cmds[:timestamp] /= @hosts.size

    # fix up age
    # FIXME: Again, none of the follwing really makes sense
    # NOTE: maybe we should also show the oldest besides average
    @slabs.each do |chunk_size, slab|
      slab[:timestamp] = Time.now.to_i
    end
  end
end

module Screen
  class << self
    def cols
      @cols
    end

    def rows
      @rows
    end

    def with_ansi(&block)
      yield unless $options[:raw]
    end

    def display(&block)
      @rows = 1000
      @cols = 80
      # FFS THIS IS TERRIBLE
      old_state = `stty -g`
      get_screen_size
      with_ansi { system "stty raw -echo" }
      yield
    ensure
      system "stty #{old_state}"
    end

    def get_screen_size
      with_ansi do
        terminfo = `stty -a`
        if terminfo =~ /rows (\d+)/
          @rows = $1.to_i
        end
        if terminfo =~ /columns (\d+)/
          @cols = $1.to_i
        end
      end
    end

    def cls
      go(0, 0)
      with_ansi do
        STDERR.write("\033[0m\033[0J")
      end
    end

    def go(x, y)
      x += @cols if x < 0
      y += @rows if y < 0
      with_ansi do
        STDERR.write("\033[#{y};#{x}H")
      end
    end

  end
end


# ----------

Signal.trap("WINCH") do
  $winch = true
end

# change the string color if a metric flows over or under given threshold
def warning_string(value, warning_level, warning_style, format=nil)
  # get the string
  s = if format
        format % value
      else
        value.to_time_string
      end

  # decide the color
  if (value >= 0 &&  # negative values were never updated
      ((warning_style == :over  && value > warning_level) ||
       (warning_style == :under && value < warning_level)))
    s.red
  else
    s
  end
end

def display
  if $winch
    Screen.get_screen_size
    $winch = false
  end

  window_height = Screen.rows - $buf.headers.size - 1
  if $buf.top_row > $buf.rows.size - window_height
    $buf.top_row = $buf.rows.size - window_height
  end
  if $buf.top_row < 0
    $buf.top_row = 0
  end
  bottom_row = [ $buf.rows.size, $buf.top_row + window_height ].min
  scrollbar = make_scrollbar(window_height, $buf.rows.size, $buf.top_row, bottom_row)
  display_rows = $buf.rows.slice($buf.top_row, window_height)
  unless $options[:raw] || (display_rows.size == $buf.rows.size)
    display_rows = display_rows.zip(scrollbar).map do |r, s|
      s + r[1..-1]
    end
  end

  Screen.cls
  $buf.headers.each { |h| STDERR.write(h + "\r\n") }
  display_rows.each { |row| STDERR.write(row + "\r\n") }
  STDERR.write("\r\n") if $options[:raw]
end

def show_help
  Screen.cls
  STDERR.print("\r\n")
  STDERR.print("Commands:\r\n")
  STDERR.print("\r\n")
  STDERR.print("  H  switch to hosts view\r\n")
  STDERR.print("  S  switch to slabs view\r\n")
  STDERR.print("  C  switch to command view\r\n")
  STDERR.print("\r\n")
  STDERR.print("  J  scroll down\r\n")
  STDERR.print("  K  scroll up\r\n")
  STDERR.print("\r\n")
  STDERR.print("  Q  quit (or ^C)\r\n")
  STDERR.print("\r\n")
  STDERR.print("Press any key to resume: ")

  while (STDIN.read(1) rescue "") == ""; end
  display
end

def check_key(seconds)
  time_left = seconds
  while time_left > 0
    c = STDIN.read_nonblock(1) rescue ""
    # do something with c
    if c == "\x03" || c == "q" || c == "Q"
      exit
    elsif c == "j" || c == "J"
      $buf.top_row += 1
      display
    elsif c == "k" || c == "K"
      $buf.top_row -= 1
      display
    elsif c == "s" || c == "S"
      $state = "slab"
      $buf.top_row = 0
      return
    elsif c == "h" || c == "H"
      $state = "host"
      $buf.top_row = 0
      return
    elsif c == "c" || c == "C"
      $state = "command"
      $buf.top_row = 0
      return
    elsif c == "?"
      show_help
    end
    display if $winch
    sleep DELAY
    time_left -= DELAY
  end
end

def make_scrollbar(scrollbar_height, total_size, view_top, view_bottom)
  scrollbar_height -= 2
  return [] if scrollbar_height <= 0 || total_size <= 0
  top_extent = (view_top.to_f / total_size * scrollbar_height + 0.5).to_i
  scrollbar_extent = (view_bottom.to_f / total_size * scrollbar_height + 0.5).to_i - top_extent
  bottom_extent = scrollbar_height - top_extent - scrollbar_extent
  rv = []
  rv << "^".cyan
  top_extent.times { rv << " " }
  scrollbar_extent.times { rv << " ".reverse.cyan }
  bottom_extent.times { rv << " " }
  rv << "v".cyan
end

def ascii_bar(width, max, bar1, bar2)
  bar1_extent = (bar1.to_f / max * width + 0.5).to_i
  bar2_extent = (bar2.to_f / max * width + 0.5).to_i - bar1_extent
  (width > 0) ? ("\#" * bar1_extent).red + ("-" * bar2_extent).green + " " * (width - bar1_extent - bar2_extent) : ""
end

def build_headers(left_header, fields, field_definitions)
  current_time = Time.now.strftime("%H:%M:%S")
  $buf.headers = []
  padding = " " * (Screen.cols - current_time.text_size - left_header.text_size - 1)
  $buf.headers << left_header + padding + current_time
  field_widths = fields.inject(0) { |sum, field| sum + field_definitions[field][:width] }
  # this only works if no more than one field is springy (width = 0)
  field_definitions.each do |name, field_definition|
    field_definition[:width] = Screen.cols - field_widths - 3 if field_definition[:width] == 0
  end
  field_widths = fields.inject(0) { |sum, field| sum + field_definitions[field][:width] }
  # left-justify the first field header
  line = fields.inject("") do |line, field|
    if (field_definitions[field][:justify] == :left)
      line + field_definitions[field][:title].ljust(field_definitions[field][:width])
    else
      line + field_definitions[field][:title].rjust(field_definitions[field][:width])
    end
  end
  $buf.headers << (line + " " * (Screen.cols - field_widths - 1)).reverse
end

def build_row(row, fields, field_definitions)
  fields.inject("") do |line, field|
    if field_definitions[field][:justify] == :left
      line + row[field].ljust(field_definitions[field][:width] + row[field].size - row[field].text_size)
    else
      line + row[field].rjust(field_definitions[field][:width] + row[field].size - row[field].text_size)
    end
  end
end

def dump_slabs(slabs, last_slabs=nil)
  slab_total = slabs.inject(0) { |n, (k, v)| n + v[:slab_curr] }
  slot_total = slabs.inject(0) { |n, (k, v)| n + (SLAB_SIZE / k) * v[:slab_curr] }
  slot_used = slabs.inject(0) { |n, (k, v)| n + v[:item_curr] }
  byte_used = slabs.inject(0) { |n, (k, v)| n + k * v[:item_curr] }
  usage = slot_used.to_f * 100 / slot_total

  slabs.each do |k, s|
    s[:mem_total] = SLAB_SIZE  * s[:slab_curr]
    s[:mem_used] = k * s[:item_curr]
  end
  mem_max = slabs.map { |k, s| s[:mem_total] }.max
  if mem_max.zero?
    mem_max = 1 # to render the in slab view properly
  end

  rates = {}
  if last_slabs
    slabs.each do |k, slab|
      last_slab = last_slabs[k]
      rate = {}
      timespan_sec = slab[:timestamp] - last_slabs[k][:timestamp]
      rate[:evict] = (slab[:evict] - last_slab[:evict]).to_f / timespan_sec
      rate[:slab_evict] = (slab[:slab_evict] - last_slab[:slab_evict]).to_f / timespan_sec
      rate[:expire] = (slab[:expire] - last_slab[:expire]).to_f / timespan_sec
      rate[:locate] = (slab[:locate] - last_slab[:locate]).to_f / timespan_sec
      rate[:insert] = (slab[:insert] - last_slab[:insert]).to_f / timespan_sec
      rates[k] =rate
    end
  else
    slabs.each do |k, slab|
      rate = {}
      rate[:evict] = slab[:evict]
      rate[:slab_evict] = slab[:slab_evict]
      rate[:expire] = slab[:expire]
      rate[:locate] = slab[:locate]
      rate[:insert] = slab[:insert]
      rates[k] =rate
    end
  end

  left_header = " - SLAB VIEW - Total Slabs:#{slab_total.to_h}; Slot Used / Allocated: "\
                "#{slot_used.to_h} /#{slot_total.to_h} (#{"%3.1f" % usage}%)"
  build_headers(left_header, SLAB_ORDER, SLAB_FIELDS)

  # build up data rows
  $buf.rows = slabs.sort.map do |k, v|
    rate = rates[k]
    item_ratio = v[:slab_curr].nonzero? ?
                 v[:item_curr].to_f * 100 / (SLAB_SIZE / k * v[:slab_curr]) : 0
    data_ratio = v[:item_curr].nonzero? ?
                 v[:data_curr].to_f * 100 / (k * v[:item_curr]) : 0
    payload_ratio = v[:data_curr].nonzero? ?
                    v[:data_value].to_f * 100 / v[:data_curr] : 0
    bar = ascii_bar(SLAB_FIELDS[:bar][:width] - 2, mem_max,
                    v[:mem_used], v[:mem_total])
    row = {
      :size         => k.to_h,
      :slab         => v[:slab_curr].to_h,
      :item         => v[:item_curr].to_h,
      :item_pct     => ("%3.1f%%" % item_ratio),
      :data_pct     => ("%3.1f%%" % data_ratio),
      :payload_pct  => ("%3.1f%%" % payload_ratio),
      :expire       => "%7.1f" % rate[:expire],
      :evict        => "%7.1f" % rate[:evict],
      :slab_evict   => "%7.1f" % rate[:slab_evict],
      :locate       => "%7.1f" % rate[:locate],
      :insert       => "%7.1f" % rate[:insert],
      :bar          => (" " + bar + " "),
    }
    SLAB_ORDER.inject("") { |line, field| line + row[field].rjust(SLAB_FIELDS[field][:width]) }
  end
  # Summaries
  bar = ascii_bar(SLAB_FIELDS[:bar][:width] - 2, mem_max, 0, 0)
  row = {
    :size         => NA,
    :slab         => slab_total.to_h,
    :item         => slot_used.to_h,
    :item_pct     => NA,
    :data_pct     => NA,
    :payload_pct  => NA,
    :expire       => rates.inject(0) { |m, (k, r)| m + r[:expire] }.to_i.to_h,
    :evict        => rates.inject(0) { |m, (k, r)| m + r[:evict] }.to_i.to_h,
    :slab_evict   => rates.inject(0) { |m, (k, r)| m + r[:slab_evict] }.to_i.to_h,
    :locate       => rates.inject(0) { |m, (k, r)| m + r[:locate] }.to_i.to_h,
    :insert       => rates.inject(0) { |m, (k, r)| m + r[:insert] }.to_i.to_h,
    :bar          => (" " + bar + " "),
  }
  $buf.rows << SLAB_ORDER.inject("") { |line, field| line + row[field].rjust(SLAB_FIELDS[field][:width]) }
  display
end

def dump_hosts(hosts, last_hosts=nil)
  mem_total = hosts.inject(0) { |m, (n, s)| m + s[:maxbytes] }
  mem_used = hosts.inject(0) { |m, (n, s)| m + s[:data] }
  total_usage = mem_total.nonzero? ? mem_used.to_f * 100.0 / mem_total : 0.0

  rates = {}
  hosts.each do |name, host|
    rate = {}
    if last_hosts
      last_host = last_hosts[name]
      timespan_sec = host[:timestamp] - last_host[:timestamp]
      rate[:expire] = (host[:expire] - last_host[:expire]).to_f / timespan_sec
      rate[:evict] = (host[:evict] - last_host[:evict]).to_f / timespan_sec
      rate[:rdata] = (host[:rdata] - last_host[:rdata]).to_f / timespan_sec
      rate[:wdata] = (host[:wdata] - last_host[:wdata]).to_f / timespan_sec
      rate[:req] = (host[:req] - last_host[:req]).to_f / timespan_sec
      rate[:svrerr] = (host[:svrerr] - last_host[:svrerr]).to_f / timespan_sec
    else
      rate[:expire] = host[:expire]
      rate[:evict] = host[:evict]
      rate[:rdata] = host[:rdata]
      rate[:wdata] = host[:wdata]
      rate[:req] = host[:req]
      rate[:svrerr] = host[:svrerr]
    end
    rates[name] = rate
  end

  left_header = " - HOST VIEW - Data size / Available memory: "\
                "#{mem_used.to_h} /#{mem_total.to_h} (#{"%3.1f" % total_usage}%)"
  build_headers(left_header, HOST_ORDER, HOST_FIELDS)

  $buf.rows = hosts.map do |host, stats|
    server, port = host.split(":")
    server = server.length > 15 ? server[0, 15] + "..." : server
    rate = rates[host]
    row = {
      :host     => "%18s:%5s" % [server, port],
      :uptime   => stats[:uptime].to_time_string,
      :util     => " %3.1f" % (stats[:data].to_f * 100.0 / stats[:maxbytes]),
      :conn     => stats[:conn].to_h,
      :latency  => warning_string(stats[:latency], 0.01, :over),
      :expire   => rate[:expire].to_i.to_h,
      :evict    => rate[:evict].to_i.to_h,
      :rdata    => rate[:rdata].to_i.to_h,
      :wdata    => rate[:wdata].to_i.to_h,
      :req      => rate[:req].to_i.to_h,
      :svrerr   => warning_string(rate[:svrerr], 0.01, :over, "%7.1f"),
    }
    build_row(row, HOST_ORDER, HOST_FIELDS)
  end
  # Summaries
  row = {
    :host     => "%3d hosts" % hosts.length,
    :uptime   => NA,
    :util     => " %3.1f" % total_usage,
    :conn     => NA,
    :latency  => NA,
    :expire   => rates.inject(0) { |m, (n, r)| m + r[:expire] }.to_i.to_h,
    :evict    => rates.inject(0) { |m, (n, r)| m + r[:evict] }.to_i.to_h,
    :rdata    => rates.inject(0) { |m, (n, r)| m + r[:rdata] }.to_i.to_h,
    :wdata    => rates.inject(0) { |m, (n, r)| m + r[:wdata] }.to_i.to_h,
    :req      => rates.inject(0) { |m, (n, r)| m + r[:req] }.to_i.to_h,
    :svrerr   => warning_string(rates.inject(0) { |m, (n, r)| m + r[:svrerr]}, 0.01, :over, "%7.1f"),
  }
  $buf.rows << build_row(row, HOST_ORDER, HOST_FIELDS)

  display
end

def dump_cmds(cmds, last_cmds=nil)
  rates = {
    :count      => {},
    :error      => {},
    :success    => {},
    :hit        => {},
    :miss       => {},
    :exist      => {},
  }
  if last_cmds
    timespan_sec = cmds[:timestamp] - last_cmds[:timestamp]
    CMD.each do |cmd|
      for category in [:count, :error, :success, :hit, :miss, :exist]
        if cmds[category][cmd]
          if timespan_sec > 0
            rates[category][cmd] =
              (cmds[category][cmd] - last_cmds[category][cmd]).to_f / timespan_sec
          else
            rates[category][cmd] = 0
          end
        end
      end
    end
  else # use absolutes
    CMD.each do |cmd|
      for category in [:count, :error, :success, :hit, :miss, :exist]
        if cmds[category][cmd]
          rates[category][cmd] =cmds[category][cmd]
        end
      end
    end
  end
  cmd_total = rates[:count].inject(0.0) { |n, (k, v)| n += v }
  successful = rates[:success].inject(rates[:hit][:delete]) { |n, (k,v)| n += v }
  cmd_error = rates[:error].inject(0.0) { |n, (k, v)| n += v }

  # successful commands are _success for most commands and _hit for delete
  if cmd_total.nonzero?
    success_rate = successful.to_f * 100 / cmd_total
  else
    success_rate = 0
  end

  left_header = " - COMMAND VIEW - Successful / Total: "\
                "#{successful.to_i.to_h} /#{cmd_total.to_i.to_h} (#{"%3.1f" % success_rate}%)"\
                "   Error: #{cmd_error.to_i.to_h}"
  build_headers(left_header, CMD_ORDER, CMD_FIELDS)

  $buf.rows = []
  CMD.each do |cmd|
    if rates[:count][cmd] > 0 && rates[:success][cmd]
      success_ratio = rates[:success][cmd] * 100 / rates[:count][cmd]
    else
      success_ratio = -1.0
    end
    if rates[:count][cmd] > 0 && rates[:hit][cmd]
      hit_ratio = rates[:hit][cmd] * 100 / rates[:count][cmd]
    else
      hit_ratio = -1.0
    end
    if rates[:count][cmd] > 0 && rates[:miss][cmd]
      miss_ratio = rates[:miss][cmd] * 100 / rates[:count][cmd]
    else
      miss_ratio = -1.0
    end
    if rates[:count][cmd] > 0 && rates[:exist][cmd]
      exist_ratio = rates[:exist][cmd] * 100 / rates[:count][cmd]
    else
      exist_ratio = -1.0
    end
    row = {
      :command  => "%8s" % cmd.id2name,
      :rate     => "%3.1f" % rates[:count][cmd],
      :mix      => "%3.1f" % (rates[:count][cmd] * 100 / cmd_total),
      :success  => (success_ratio > 0) ? warning_string(success_ratio, 90, :under, "%3.1f") : NA,
      :hit      => (hit_ratio > 0) ? warning_string(hit_ratio, 90, :under, "%3.1f") : NA,
      :miss     => (miss_ratio > 0) ? "%3.1f" % miss_ratio : NA,
      :exist    => (exist_ratio > 0) ? "%3.1f" % exist_ratio : NA,
      :error    => warning_string(rates[:error][cmd], 0, :over, "%7.1f"),
    }
    $buf.rows << build_row(row, CMD_ORDER, CMD_FIELDS)
  end

  # Summaries: read, write, total
  if last_cmds
    read            = CMD_READ.inject(0.0) { |r, c| r += rates[:count][c] }
    write           = CMD_WRITE.inject(0.0) { |r, c| r += rates[:count][c] }
    read_hit        = CMD_READ.inject(0.0) { |r, c| r += rates[:hit][c] }
    write_success   = CMD_WRITE.inject(0.0) { |r, c| r += rates[:success][c] }
    read_error      = CMD_READ.inject(0.0) { |r, c| r += rates[:error][c] }
    write_error     = CMD_WRITE.inject(0.0) { |r, c| r += rates[:error][c] }

    row = {
      :command  => "READ",
      :rate     => "%3.1f" % read,
      :mix      => "%3.1f" % ( read * 100 / cmd_total),
      :success  => NA,
      :hit      => warning_string(read_hit * 100 / read, 90, :under, "%3.1f"),
      :miss     => NA,
      :exist    => NA,
      :error    => warning_string(read_error, 0, :over, "%7.1f"),
    }
    $buf.rows << build_row(row, CMD_ORDER, CMD_FIELDS)
    row = {
      :command  => "WRITE",
      :rate     => "%3.1f" % write,
      :mix      => "%3.1f" % ( write * 100 / cmd_total),
      :success  => warning_string(write_success * 100 / write, 99.9, :under, "%3.1f"),
      :hit      => NA,
      :miss     => NA,
      :exist    => NA,
      :error    => warning_string(write_error, 0, :over, "%7.1f"),
    }
    $buf.rows << build_row(row, CMD_ORDER, CMD_FIELDS)
    row = {
      :command  => "TOTAL",
      :rate     => "%3.1f" % cmd_total,
      :mix      => NA,
      :success  => NA,
      :hit      => NA,
      :miss     => NA,
      :exist    => NA,
      :error    => "%3.1f" % cmd_error,
    }
    $buf.rows << build_row(row, CMD_ORDER, CMD_FIELDS)
  end

  display
end

def load_twemcache_config(filename)
  config = YAML.load_file(filename)
  blob = if config[$options[:env]]
    config[$options[:env]]
  elsif config["timeline"] && config["timeline"][$options[:env]]
    config["timeline"][$options[:env]]
  else
    raise "Can't parse yaml for env #{$options[:env]}"
  end
  if blob["servers"]
    $options[:hosts] = blob["servers"].map { |item| item.gsub(/(\S+(?::\d+)):.*/, "\\1") }
  else
    raise "No servers!"
  end
end

def parse_args
  $options = {
    :hosts => [ "localhost:11211" ],
    :env => "production",
    :sleep => 0,
    :raw => false,
    :view => "slab", # which mode we're showing
  }

  opts = OptionParser.new do |opts|
    opts.banner = "Usage: mctop [options] [twemcache-config]"

    opts.on("-D", "--dev", "use development config") do
      $options[:env] = "development"
    end
    opts.on("-e", "--env NAME", "use specific environment (like test)") do |env|
      $options[:env] = env
    end
    opts.on("-H", "--host HOST", "connect to one or more specified instance (comma separated, port ranges with a hyphen e.g. host_1:8000-8004,host_2:8000)") do |host|
      hosts = host.split(/\s*?,\s*?/)
      # Expand port ranges for each host if specified
      hosts.map! do |uri|
        host, port = uri.split(':')
        start_port, end_port = port.split('-')
        if end_port
          (start_port..end_port).map do |port|
            "#{host}:#{port}"
          end
        else
          uri
        end
      end

      $options[:hosts] = hosts.flatten
    end
    opts.on("-s", "--sleep N", "stay running, dumping new stats every N seconds") do |n|
      $options[:sleep] = n.to_i
    end
    opts.on("-r", "--raw", "don't use ansi/vt100 codes") do
      $options[:raw] = true
    end
    opts.on("-v", "--view VIEW", "choose among [slab|host|command]") do |view|
      $options[:view] = view
    end

    opts.on_tail("-h", "--help", "show this message") do
      puts opts
      exit
    end
  end

  opts.parse!(ARGV)
  load_twemcache_config(ARGV.shift) if ARGV.size > 0
end

parse_args
$state = $options[:view]
last_summary = nil
Screen.display do
  begin
    elapsed = Benchmark.realtime do
      host_stats_arr =
        $options[:hosts].inject([]) { |list, host| list << ServerStats.new(host) }
      summary = SummaryStats.new(host_stats_arr)

      if $state == "slab"
        dump_slabs( summary.slabs, last_summary ? last_summary.slabs : nil )
      elsif $state == "host"
        dump_hosts( summary.hosts, last_summary ? last_summary.hosts : nil )
      elsif $state == "command"
        dump_cmds( summary.cmds, last_summary ? last_summary.cmds : nil )
      else
        puts "Invalid viewing mode"
        exit
      end
      last_summary = summary
    end
    check_key($options[:sleep] > elapsed ? $options[:sleep] - elapsed : 0.1 )
  end while $options[:sleep] > 0
end
