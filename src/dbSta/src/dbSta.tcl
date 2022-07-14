############################################################################
##
## Copyright (c) 2019, The Regents of the University of California
## All rights reserved.
##
## BSD 3-Clause License
##
## Redistribution and use in source and binary forms, with or without
## modification, are permitted provided that the following conditions are met:
##
## * Redistributions of source code must retain the above copyright notice, this
##   list of conditions and the following disclaimer.
##
## * Redistributions in binary form must reproduce the above copyright notice,
##   this list of conditions and the following disclaimer in the documentation
##   and/or other materials provided with the distribution.
##
## * Neither the name of the copyright holder nor the names of its
##   contributors may be used to endorse or promote products derived from
##   this software without specific prior written permission.
##
## THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
## AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
## IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
## ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
## LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
## CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
## SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
## INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
## CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
## ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
## POSSIBILITY OF SUCH DAMAGE.
##
############################################################################

namespace eval sta {

define_cmd_args "highlight_path" {[-min|-max] pin ^|r|rise|v|f|fall}

proc highlight_path { args } {
  parse_key_args "highlight_path" args keys {} \
    flags {-max -min} 0

  if { [info exists flags(-min)] && [info exists flags(-max)] } {
    sta_error "-min and -max cannot both be specified."
  } elseif [info exists flags(-min)] {
    set min_max "min"
  } elseif [info exists flags(-max)] {
    set min_max "max"
  } else {
    # Default to max path.
    set min_max "max"
  }
  if { [llength $args] == 0 } {
    highlight_path_cmd "NULL"
  } else {
    check_argc_eq2 "highlight_path" $args

    set pin_arg [lindex $args 0]
    set tr [parse_rise_fall_arg [lindex $args 1]]

    set pin [get_port_pin_error "pin" $pin_arg]
    if { [$pin is_hierarchical] } {
      sta_error "pin '$pin_arg' is hierarchical."
    } else {
      foreach vertex [$pin vertices] {
        if { $vertex != "NULL" } {
          set worst_path [vertex_worst_arrival_path_rf $vertex $tr $min_max]
          if { $worst_path != "NULL" } {
            highlight_path_cmd $worst_path
            delete_path_ref $worst_path
          }
        }
      }
    }
  }
}

# redefine sta::sta_warn/error to call utl::warn/error
proc sta_error { id msg } {
  utl::error STA $id $msg
}

proc sta_warn { id msg } {
  utl::warn STA $id $msg
}

define_cmd_args "report_units_metric" {}
proc report_units_metric { args } {

  utl::push_metrics_stage "run__flow__platform__{}_units"

  foreach unit {{"time" "timing"} {"power" "power"} {"distance" "distance"}} {
    set utype [lindex $unit 0]
    set umetric [lindex $unit 1]
    utl::metric "$umetric" "[unit_suffix $utype]"
  }

  utl::pop_metrics_stage
}

define_cmd_args "report_tns_metric" {}
proc report_tns_metric { args } {
  global sta_report_default_digits

  utl::metric_float "timing__setup__tns"  "[format_time [total_negative_slack_cmd "max"] $sta_report_default_digits]"
}


define_cmd_args "report_worst_slack_metric" {}
proc report_worst_slack_metric { args } {
  global sta_report_default_digits

  utl::metric_float "timing__setup__ws" "[format_time [worst_slack_cmd "max"] $sta_report_default_digits]"
}

define_cmd_args "report_erc_metrics" {}
proc report_erc_metrics { args } {

    set max_slew_limit [sta::max_slew_check_slack_limit]
    set max_cap_limit [sta::max_capacitance_check_slack_limit]
    set max_fanout_limit [sta::max_fanout_check_limit]
    set max_slew_violation [sta::max_slew_violation_count]
    set max_cap_violation [sta::max_capacitance_violation_count]
    set max_fanout_violation [sta::max_fanout_violation_count]

    utl::metric_float "timing__drv__max_slew_limit" $max_slew_limit
    utl::metric_int "timing__drv__max_slew" $max_slew_violation
    utl::metric_float "timing__drv__max_cap_limit" $max_cap_limit
    utl::metric_int "timing__drv__max_cap" $max_cap_violation
    utl::metric_float "timing__drv__max_fanout_limit" $max_fanout_limit
    utl::metric_int "timing__drv__max_fanout" $max_fanout_violation
}


define_cmd_args "report_power_design_metric" {}
proc report_power_design_metric { corner digits } {
  set power_result [design_power $corner]
  set totals        [lrange $power_result  0  3]
  lassign $totals design_internal design_switching design_leakage design_total

  utl::metric_float "power__internal__total" $design_internal
  utl::metric_float "power__switchng__total" $design_switching
  utl::metric_float "power__leakage__total" $design_leakage
  utl::metric_float "power__total" $design_total
}

define_cmd_args "report_design_area_metrics" {}
proc report_design_area_metrics {args} {
  set db [::ord::get_db]
  set dbu_per_uu [[$db getTech] getDbUnitsPerMicron]
  set block [[$db getChip] getBlock]

  set num_ios [llength [$block getBTerms]]

  set num_insts 0
  set num_stdcells 0
  set num_macros 0
  set total_area 0.0
  set macro_area 0.0
  set stdcell_area 0.0

  foreach inst [$block getInsts] {
    set inst_master [$inst getMaster]
    if {[$inst_master isFiller]} {
      continue
    }
    set wid [$inst_master getWidth]
    set ht [$inst_master getHeight]
    set inst_area  [expr $wid * $ht]
    set total_area [expr $total_area + $inst_area] 
    set num_insts [expr $num_insts + 1]
    if { [string match [$inst_master getType] "BLOCK"] } {
      set num_macros [expr $num_macros + 1]
      set macro_area [expr $macro_area + $inst_area]
    } else {
      set num_stdcells [expr $num_stdcells + 1]
      set stdcell_area [expr $stdcell_area + $inst_area]
    }
  }

  set total_area [expr $total_area / [expr $dbu_per_uu * $dbu_per_uu]]
  set stdcell_area [expr $stdcell_area / [expr $dbu_per_uu * $dbu_per_uu]]
  set macro_area [expr $macro_area / [expr $dbu_per_uu * $dbu_per_uu]]

  utl::metric_int "design__io" $num_ios
  utl::metric_int "design__instance__count" $num_insts
  utl::metric_float "design__instance__area" $total_area
  utl::metric_int "design__instance__count__stdcell" $num_stdcells
  utl::metric_float "design__instance__area__stdcell" $stdcell_area
  utl::metric_int "design__instance__count__macros" $num_macros
  utl::metric_float "design__instance__area__macros" $macro_area
}

# namespace
}
