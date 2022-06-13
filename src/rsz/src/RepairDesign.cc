/////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2022, The Regents of the University of California
// All rights reserved.
//
// BSD 3-Clause License
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////////

#include "RepairDesign.hh"
#include "rsz/Resizer.hh"
#include "BufferedNet.hh"

#include "db_sta/dbNetwork.hh"

#include "sta/Units.hh"
#include "sta/PortDirection.hh"
#include "sta/Liberty.hh"
#include "sta/Graph.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/Sdc.hh"
#include "sta/Corner.hh"
#include "sta/PathVertex.hh"
#include "sta/SearchPred.hh"
#include "sta/Search.hh"
#include "sta/PathRef.hh"
#include "sta/PathExpanded.hh"
#include "sta/Fuzzy.hh"

namespace rsz {

using std::abs;
using std::min;
using std::max;

using utl::RSZ;

using sta::Port;
using sta::NetPinIterator;
using sta::InstancePinIterator;
using sta::NetConnectedPinIterator;
using sta::NetIterator;
using sta::Clock;
using sta::INF;

RepairDesign::RepairDesign(Resizer *resizer) :
  StaState(),
  logger_(nullptr),
  sta_(nullptr),
  db_network_(nullptr),
  resizer_(resizer),
  dbu_(0),
  corner_(nullptr),
  resize_count_(0),
  inserted_buffer_count_(0),
  min_(MinMax::min()),
  max_(MinMax::max())
{
}

void
RepairDesign::init()
{
  logger_ = resizer_->logger_;
  sta_ = resizer_->sta_;
  db_network_ = resizer_->db_network_;
  dbu_ = resizer_->dbu_;

  copyState(sta_);
}

// Repair long wires, max slew, max capacitance, max fanout violations
// The whole enchilada.
// max_wire_length zero for none (meters)
void
RepairDesign::repairDesign(double max_wire_length,
                           double slew_margin,
                           double max_cap_margin)
{
  init();
  int repaired_net_count, slew_violations, cap_violations;
  int fanout_violations, length_violations;
  repairDesign(max_wire_length, slew_margin, max_cap_margin,
               repaired_net_count, slew_violations, cap_violations,
               fanout_violations, length_violations);

  if (slew_violations > 0)
    logger_->info(RSZ, 34, "Found {} slew violations.", slew_violations);
  if (fanout_violations > 0)
    logger_->info(RSZ, 35, "Found {} fanout violations.", fanout_violations);
  if (cap_violations > 0)
    logger_->info(RSZ, 36, "Found {} capacitance violations.", cap_violations);
  if (length_violations > 0)
    logger_->info(RSZ, 37, "Found {} long wires.", length_violations);
  if (inserted_buffer_count_ > 0)
    logger_->info(RSZ, 38, "Inserted {} buffers in {} nets.",
                  inserted_buffer_count_,
                  repaired_net_count);
  if (resize_count_ > 0)
    logger_->info(RSZ, 39, "Resized {} instances.", resize_count_);
}

void
RepairDesign::repairDesign(double max_wire_length, // zero for none (meters)
                           double slew_margin,
                           double max_cap_margin,
                           int &repaired_net_count,
                           int &slew_violations,
                           int &cap_violations,
                           int &fanout_violations,
                           int &length_violations)
{
  init();

  slew_violations = 0;
  cap_violations = 0;
  fanout_violations = 0;
  length_violations = 0;
  repaired_net_count = 0;
  inserted_buffer_count_ = 0;
  resize_count_ = 0;
  resizer_->resized_multi_output_insts_.clear();

  sta_->checkSlewLimitPreamble();
  sta_->checkCapacitanceLimitPreamble();
  sta_->checkFanoutLimitPreamble();

  resizer_->incrementalParasiticsBegin();
  int max_length = resizer_->metersToDbu(max_wire_length);
  for (int i = resizer_->level_drvr_vertices_.size() - 1; i >= 0; i--) {
    Vertex *drvr = resizer_->level_drvr_vertices_[i];
    Pin *drvr_pin = drvr->pin();
    Net *net = network_->isTopLevelPort(drvr_pin)
      ? network_->net(network_->term(drvr_pin))
      : network_->net(drvr_pin);
    bool debug = (drvr_pin == resizer_->debug_pin_);
    if (debug)
      logger_->setDebugLevel(RSZ, "repair_net", 3);
    if (net
        && !sta_->isClock(drvr_pin)
        // Exclude tie hi/low cells and supply nets.
        && !drvr->isConstant())
      repairNet(net, drvr_pin, drvr, slew_margin, max_cap_margin,
                true, true, true, max_length, true,
                repaired_net_count, slew_violations, cap_violations,
                fanout_violations, length_violations);
    if (debug)
      logger_->setDebugLevel(RSZ, "repair_net", 0);
  }
  resizer_->updateParasitics();
  resizer_->incrementalParasiticsEnd();

  if (inserted_buffer_count_ > 0)
    resizer_->level_drvr_vertices_valid_ = false;
}

// Repair long wires from clock input pins to clock tree root buffer
// because CTS ignores the issue.
// no max_fanout/max_cap checks.
// Use max_wire_length zero for none (meters)
void
RepairDesign::repairClkNets(double max_wire_length)
{
  init();
  // Need slews to resize inserted buffers.
  sta_->findDelays();

  int slew_violations = 0;
  int cap_violations = 0;
  int fanout_violations = 0;
  int length_violations = 0;
  int repaired_net_count = 0;
  inserted_buffer_count_ = 0;
  resize_count_ = 0;
  resizer_->resized_multi_output_insts_.clear();

  resizer_->incrementalParasiticsBegin();
  int max_length = resizer_->metersToDbu(max_wire_length);
  for (Clock *clk : sdc_->clks()) {
    const PinSet *clk_pins = sta_->pins(clk);
    if (clk_pins) {
      for (const Pin *clk_pin : *clk_pins) {
        Net *net = network_->isTopLevelPort(clk_pin)
          ? network_->net(network_->term(clk_pin))
          : network_->net(clk_pin);
        if (network_->isDriver(clk_pin)) {
          Vertex *drvr = graph_->pinDrvrVertex(clk_pin);
          // Do not resize clock tree gates.
          repairNet(net, clk_pin, drvr, 0.0, 0.0,
                    false, false, false, max_length, false,
                    repaired_net_count, slew_violations, cap_violations,
                    fanout_violations, length_violations);
        }
      }
    }
  }
  resizer_->updateParasitics();
  resizer_->incrementalParasiticsEnd();

  if (length_violations > 0)
    logger_->info(RSZ, 47, "Found {} long wires.", length_violations);
  if (inserted_buffer_count_ > 0) {
    logger_->info(RSZ, 48, "Inserted {} buffers in {} nets.",
                  inserted_buffer_count_,
                  repaired_net_count);
    resizer_->level_drvr_vertices_valid_ = false;
  }
}

// Repair one net (for debugging)
void
RepairDesign::repairNet(Net *net,
                        double max_wire_length, // meters
                        double slew_margin,
                        double max_cap_margin)
{
  init();

  int slew_violations = 0;
  int cap_violations = 0;
  int fanout_violations = 0;
  int length_violations = 0;
  int repaired_net_count = 0;
  inserted_buffer_count_ = 0;
  resize_count_ = 0;
  resizer_->resized_multi_output_insts_.clear();
  resizer_->buffer_moved_into_core_ = false;

  sta_->checkSlewLimitPreamble();
  sta_->checkCapacitanceLimitPreamble();
  sta_->checkFanoutLimitPreamble();

  resizer_->incrementalParasiticsBegin();
  int max_length = resizer_->metersToDbu(max_wire_length);
  PinSet *drivers = network_->drivers(net);
  if (drivers && !drivers->empty()) {
    PinSet::Iterator drvr_iter(drivers);
    Pin *drvr_pin = drvr_iter.next();
    Vertex *drvr = graph_->pinDrvrVertex(drvr_pin);
    repairNet(net, drvr_pin, drvr, slew_margin, max_cap_margin,
              true, true, true, max_length, true,
              repaired_net_count, slew_violations, cap_violations,
              fanout_violations, length_violations);
  }
  resizer_->updateParasitics();
  resizer_->incrementalParasiticsEnd();

  if (slew_violations > 0)
    logger_->info(RSZ, 51, "Found {} slew violations.", slew_violations);
  if (fanout_violations > 0)
    logger_->info(RSZ, 52, "Found {} fanout violations.", fanout_violations);
  if (cap_violations > 0)
    logger_->info(RSZ, 53, "Found {} capacitance violations.", cap_violations);
  if (length_violations > 0)
    logger_->info(RSZ, 54, "Found {} long wires.", length_violations);
  if (inserted_buffer_count_ > 0) {
    logger_->info(RSZ, 55, "Inserted {} buffers in {} nets.",
                  inserted_buffer_count_,
                  repaired_net_count);
    resizer_->level_drvr_vertices_valid_ = false;
  }
  if (resize_count_ > 0)
    logger_->info(RSZ, 56, "Resized {} instances.", resize_count_);
  if (resize_count_ > 0)
    logger_->info(RSZ, 57, "Resized {} instances.", resize_count_);
}

void
RepairDesign::repairNet(Net *net,
                        const Pin *drvr_pin,
                        Vertex *drvr,
                        double slew_margin,
                        double max_cap_margin,
                        bool check_slew,
                        bool check_cap,
                        bool check_fanout,
                        int max_length, // dbu
                        bool resize_drvr,
                        int &repaired_net_count,
                        int &slew_violations,
                        int &cap_violations,
                        int &fanout_violations,
                        int &length_violations)
{
  // Hands off special nets.
  if (!db_network_->isSpecial(net)) {
    debugPrint(logger_, RSZ, "repair_net", 1, "repair net {}",
               sdc_network_->pathName(drvr_pin));
    // Resize the driver to normalize slews before repairing limit violations.
    if (resize_drvr)
      resize_count_ += resizer_->resizeToTargetSlew(drvr_pin);
    // For tristate nets all we can do is resize the driver.
    if (!resizer_->isTristateDriver(drvr_pin)) {
      const Corner *corner = sta_->cmdCorner();
      BufferedNetPtr bnet = resizer_->makeBufferedNetSteiner(drvr_pin, corner);
      if (bnet) {
        resizer_->ensureWireParasitic(drvr_pin, net);
        graph_delay_calc_->findDelays(drvr);

        float max_cap = INF;
        float max_fanout = INF;
        bool repair_slew = false;
        bool repair_cap = false;
        bool repair_fanout = false;
        bool repair_wire = false;
        if (check_cap) {
          float cap1, max_cap1, cap_slack1;
          const Corner *corner1;
          const RiseFall *tr1;
          sta_->checkCapacitance(drvr_pin, nullptr, max_,
                                 corner1, tr1, cap1, max_cap1, cap_slack1);
          if (max_cap1 > 0.0 && corner1) {
            max_cap1 *= (1.0 - max_cap_margin / 100.0);
            max_cap = max_cap1;
            if (cap1 > max_cap1) {
              corner = corner1;
              cap_violations++;
              repair_cap = true;
            }
          }
        }
        if (check_fanout) {
          float fanout, fanout_slack;
          sta_->checkFanout(drvr_pin, max_,
                            fanout, max_fanout, fanout_slack);
          if (max_fanout > 0.0 && fanout_slack < 0.0) {
            fanout_violations++;
            repair_fanout = true;
          }
        }
        int wire_length = bnet->maxLoadWireLength();
        if (max_length
            && wire_length > max_length) {
          length_violations++;
          repair_wire = true;
        }
        if (check_slew) {
          float slew1, slew_slack1, max_slew1;
          const Corner *corner1;
          // Check slew at the driver.
          checkSlew(drvr_pin, slew_margin, slew1, max_slew1, slew_slack1, corner1);
          // Max slew violations at the driver pin are repaired by reducing the
          // load capacitance. Wire resistance may shield capacitance from the
          // driver but so this is conservative.
          // Find max load cap that corresponds to max_slew.
          LibertyPort *drvr_port = network_->libertyPort(drvr_pin);
          if (corner1
              && max_slew1 > 0.0) {
            if (drvr_port) {
              float max_cap1 = findSlewLoadCap(drvr_port, max_slew1, corner1);
              max_cap = min(max_cap, max_cap1);
            }
            corner = corner1;
            if (slew_slack1 < 0.0) {
              debugPrint(logger_, RSZ, "repair_net", 2,
                         "drvr slew violation slew={} max_slew={}",
                         delayAsString(slew1, this, 3),
                         delayAsString(max_slew1, this, 3));
              repair_slew = true;
              slew_violations++;
            }
          }
          // Check slew at the loads.
          // Note that many liberty libraries do not have max_transition attributes on
          // input pins.
          // Max slew violations at the load pins are repaired by inserting buffers
          // and reducing the wire length to the load.
          resizer_->checkLoadSlews(drvr_pin, slew_margin, slew1,
                                   max_slew1, slew_slack1, corner1);
          if (slew_slack1 < 0.0) {
            debugPrint(logger_, RSZ, "repair_net", 2,
                       "load slew violation load_slew={} max_slew={}",
                       delayAsString(slew1, this, 3),
                       delayAsString(max_slew1, this, 3));
            corner = corner1;
            // Don't double count the driver/load on same net.
            if (!repair_slew)
              slew_violations++;
            repair_slew = true;
          }
        }
        if (repair_slew
            || repair_cap
            || repair_fanout
            || repair_wire) {
          Point drvr_loc = db_network_->location(drvr->pin());
          debugPrint(logger_, RSZ, "repair_net", 1, "driver {} ({} {}) l={}",
                     sdc_network_->pathName(drvr_pin),
                     units_->distanceUnit()->asString(dbuToMeters(drvr_loc.getX()), 1),
                     units_->distanceUnit()->asString(dbuToMeters(drvr_loc.getY()), 1),
                     units_->distanceUnit()->asString(dbuToMeters(wire_length), 1));
          repairNet(bnet, drvr_pin, max_cap, max_fanout, max_length, corner);
          repaired_net_count++;

          if (resize_drvr)
            resize_count_ += resizer_->resizeToTargetSlew(drvr_pin);
        }
      }
    }
  }
}

bool
RepairDesign::checkLimits(const Pin *drvr_pin,
                          double slew_margin,
                          double max_cap_margin,
                          bool check_slew,
                          bool check_cap,
                          bool check_fanout)
{
  if (check_cap) {
    float cap1, max_cap1, cap_slack1;
    const Corner *corner1;
    const RiseFall *tr1;
    sta_->checkCapacitance(drvr_pin, nullptr, max_,
                           corner1, tr1, cap1, max_cap1, cap_slack1);
    max_cap1 *= (1.0 - max_cap_margin / 100.0);
    if (cap1 < max_cap1)
      return true;
  }
  if (check_fanout) {
    float fanout, fanout_slack, max_fanout;
    sta_->checkFanout(drvr_pin, max_,
                      fanout, max_fanout, fanout_slack);
    if (fanout_slack < 0.0)
      return true;

  }
  if (check_slew) {
    float slew1, slew_slack1, max_slew1;
    const Corner *corner1;
    checkSlew(drvr_pin, slew_margin, slew1, max_slew1, slew_slack1, corner1);
    if (slew_slack1 < 0.0)
      return true;
    resizer_->checkLoadSlews(drvr_pin, slew_margin, slew1,
                             max_slew1, slew_slack1, corner1);
    if (slew_slack1 < 0.0)
      return true;
  }
  return false;
}

void
RepairDesign::checkSlew(const Pin *drvr_pin,
                        double slew_margin,
                        // Return values.
                        Slew &slew,
                        float &limit,
                        float &slack,
                        const Corner *&corner)
{
  slack = INF;
  limit = INF;
  corner = nullptr;

  const Corner *corner1;
  const RiseFall *tr1;
  Slew slew1;
  float limit1, slack1;
  sta_->checkSlew(drvr_pin, nullptr, max_, false,
                  corner1, tr1, slew1, limit1, slack1);
  if (corner1) {
    limit1 *= (1.0 - slew_margin / 100.0);
    slack1 = limit1 - slew1;
    if (slack1 < slack) {
      slew = slew1;
      limit = limit1;
      slack = slack1;
      corner = corner1;
    }
  }
}

float
RepairDesign::bufferInputMaxSlew(LibertyCell *buffer,
                                 const Corner *corner) const
{
  LibertyPort *input, *output;
  buffer->bufferPorts(input, output);
  return resizer_->maxInputSlew(input, corner);
}

// Find the output port load capacitance that results in slew.
double
RepairDesign::findSlewLoadCap(LibertyPort *drvr_port,
                              double slew,
                              const Corner *corner)
{
  const DcalcAnalysisPt *dcalc_ap = corner->findDcalcAnalysisPt(max_);
  double drvr_res = drvr_port->driveResistance();
  if (drvr_res == 0.0)
    return INF;
  // cap1 lower bound
  // cap2 upper bound
  double cap1 = 0.0;
  double cap2 = slew / drvr_res * 2;
  double tol = .01; // 1%
  double diff1 = gateSlewDiff(drvr_port, cap2, slew, dcalc_ap);
  // binary search for diff = 0.
  while (abs(cap1 - cap2) > max(cap1, cap2) * tol) {
    if (diff1 < 0.0) {
      cap1 = cap2;
      cap2 *= 2;
      diff1 = gateSlewDiff(drvr_port, cap2, slew, dcalc_ap);
    }
    else {
      double cap3 = (cap1 + cap2) / 2.0;
      double diff2 = gateSlewDiff(drvr_port, cap3, slew, dcalc_ap);
      if (diff2 < 0.0) {
        cap1 = cap3;
      }
      else {
        cap2 = cap3;
        diff1 = diff2;
      }
    }
  }
  return cap1;
}

// objective function
double
RepairDesign::gateSlewDiff(LibertyPort *drvr_port,
                           double load_cap,
                           double slew,
                           const DcalcAnalysisPt *dcalc_ap)
{
  ArcDelay delays[RiseFall::index_count];
  Slew slews[RiseFall::index_count];
  resizer_->gateDelays(drvr_port, load_cap, dcalc_ap, delays, slews);
  Slew gate_slew = max(slews[RiseFall::riseIndex()], slews[RiseFall::fallIndex()]);
  return gate_slew - slew;
}

void
RepairDesign::repairNet(BufferedNetPtr bnet,
                        const Pin *drvr_pin,
                        float max_cap,
                        float max_fanout,
                        int max_length, // dbu
                        const Corner *corner)
{
  drvr_pin_ = drvr_pin;
  max_cap_ = max_cap;
  max_fanout_ = max_fanout;
  max_length_ = max_length;
  corner_ = corner;

  int wire_length;
  PinSeq load_pins;
  repairNet(bnet, 0, wire_length, load_pins);
}

void
RepairDesign::repairNet(BufferedNetPtr bnet,
                        int level,
                        // Return values.
                        // Remaining parasiics after repeater insertion.
                        int &wire_length, // dbu
                        PinSeq &load_pins)
{
  switch (bnet->type()) {
  case BufferedNetType::wire:
    repairNetWire(bnet, level, wire_length, load_pins);
    break;
  case BufferedNetType::junction:
    repairNetJunc(bnet, level, wire_length, load_pins);
    break;
  case BufferedNetType::load:
    repairNetLoad(bnet, level, wire_length, load_pins);
    break;
  case BufferedNetType::buffer:
    logger_->critical(RSZ, 72, "unhandled BufferedNet type");
    break;
  }
}

void
RepairDesign::repairNetWire(BufferedNetPtr bnet,
                            int level,
                            // Return values.
                            // Remaining parasiics after repeater insertion.
                            int &wire_length, // dbu
                            PinSeq &load_pins)
{
  debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}{}",
                 "", level,
             bnet->to_string(resizer_));
  repairNet(bnet->ref(), level+1, wire_length, load_pins);
  float max_load_slew = bnet->maxLoadSlew();

  Point to_loc = bnet->ref()->location();
  int to_x = to_loc.getX();
  int to_y = to_loc.getY();
  Point from_loc = bnet->location();
  int length = Point::manhattanDistance(from_loc, to_loc);
  wire_length += length;
  // Back up from pt to from_pt adding repeaters as necessary for
  // length/max_cap/max_slew violations.
  int from_x = from_loc.getX();
  int from_y = from_loc.getY();
  debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}wl={} l={}",
             "", level,
             units_->distanceUnit()->asString(dbuToMeters(wire_length), 1),
             units_->distanceUnit()->asString(dbuToMeters(length), 1));
  double length1 = dbuToMeters(length);
  double wire_res, wire_cap;
  bnet->wireRC(corner_, resizer_, wire_res, wire_cap);
  double load_cap = length1 * wire_cap + bnet->ref()->cap();

  float r_drvr = resizer_->driveResistance(drvr_pin_);
  double load_slew = (r_drvr + length1 * wire_res) *
    load_cap * elmore_skew_factor_;
  debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}load_slew={} r_drvr={}",
             "", level,
             delayAsString(load_slew, this, 3),
             units_->resistanceUnit()->asString(r_drvr, 3));

  LibertyCell *buffer_cell = resizer_->findTargetCell(resizer_->buffer_lowest_drive_,
                                                      load_cap, false);
  bnet->setCapacitance(load_cap);
  bnet->setFanout(bnet->ref()->fanout());
  float ref_cap = bnet->ref()->cap();

  while ((max_length_ > 0 && wire_length > max_length_)
         || (wire_cap > 0.0
             && load_cap > max_cap_)
         || load_slew > max_load_slew) {
    // Make the wire a bit shorter than necessary to allow for
    // offset from instance origin to pin and detailed placement movement.
    static double length_margin = .05;
    bool split_wire = false;
    bool resize = true;
    // Distance from repeater to ref_.
    int split_length = std::numeric_limits<int>::max();
    if (max_length_ > 0 && wire_length > max_length_) {
      debugPrint(logger_, RSZ, "repair_net", 3,
                 "{:{}s}max wire length violation {} > {}",
                 "", level,
                 units_->distanceUnit()->asString(dbuToMeters(wire_length), 1),
                 units_->distanceUnit()->asString(dbuToMeters(max_length_), 1));
      split_length = min(split_length, max_length_);
      split_wire = true;
    }
    if (wire_cap > 0.0
        && load_cap > max_cap_) {
      debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}max cap violation {} > {}",
                 "", level,
                 units_->capacitanceUnit()->asString(load_cap, 3),
                 units_->capacitanceUnit()->asString(max_cap_, 3));
      split_length = min(split_length,
                         metersToDbu((max_cap_ - ref_cap) / wire_cap));
      split_wire = true;
    }
    if (load_slew > max_load_slew) { 
      debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}max load slew violation {} > {}",
                 "", level,
                 delayAsString(load_slew, this, 3),
                 delayAsString(max_load_slew, this, 3));
      // Using elmore delay to approximate wire
      // load_slew = (Rbuffer + L*Rwire) * (L*Cwire + Cref) * elmore_skew_factor_
      // Setting this to max_load_slew is a quadratic in L
      // L^2*Rwire*Cwire + L*(Rbuffer*Cwire + Rwire*Cref)
      //   + Rbuffer*Cref - max_load_slew/elmore_skew_factor_
      // Solve using quadradic eqn for L.
      float r_buffer = resizer_->bufferDriveResistance(buffer_cell);
      float ref_cap = bnet->ref()->cap();
      float a = wire_res * wire_cap;
      float b = r_buffer * wire_cap + wire_res * ref_cap;
      float c = r_buffer * ref_cap - max_load_slew / elmore_skew_factor_;
      float l = (-b + sqrt(b*b - 4 * a * c)) / (2 * a);
      if (l >= 0.0) {
        split_length = min(split_length, metersToDbu(l));
        split_wire = true;
        resize = false;
      }
      else {
        split_length = 0;
        split_wire = true;
        resize = false;
      }
      debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}split length={}",
                 "", level,
                 units_->distanceUnit()->asString(dbuToMeters(split_length), 1));
    }
    if (split_wire) {
      // Distance from to_pt to repeater backward toward from_pt.
      // Note that split_length can be longer than the wire length
      // because it is the maximum value that satisfies max slew/cap.
      double buf_dist = (split_length >= length)
        ? length
        : split_length * (1.0 - length_margin);
      double dx = from_x - to_x;
      double dy = from_y - to_y;
      double d = (length == 0) ? 0.0 : buf_dist / length;
      int buf_x = to_x + d * dx;
      int buf_y = to_y + d * dy;
      float repeater_cap, repeater_fanout;
      makeRepeater("wire", buf_x, buf_y, buffer_cell, resize, level,
                   load_pins, repeater_cap, repeater_fanout, max_load_slew);
      // Update for the next round.
      length -= buf_dist;
      wire_length = length;
      to_x = buf_x;
      to_y = buf_y;

      float length1 = dbuToMeters(length);
      load_cap = repeater_cap + length1 * wire_cap;
      ref_cap = repeater_cap;
      load_slew = (r_drvr + length1 * wire_res) * load_cap * elmore_skew_factor_;
      buffer_cell = resizer_->findTargetCell(resizer_->buffer_lowest_drive_,
                                             load_cap, false);

      LibertyPort *input, *output;
      buffer_cell->bufferPorts(input, output);
      bnet->setCapacitance(load_cap);
      bnet->setFanout(repeater_fanout);
      bnet->setMaxLoadSlew(max_load_slew);

      debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}l={}",
                 "", level,
                 units_->distanceUnit()->asString(length1, 1));
    }
    else
      break;
  }
}

void
RepairDesign::repairNetJunc(BufferedNetPtr bnet,
                            int level,
                            // Return values.
                            // Remaining parasiics after repeater insertion.
                            int &wire_length, // dbu
                            PinSeq &load_pins)
{
  debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}{}",
                 "", level,
             bnet->to_string(resizer_));
  Point loc = bnet->location();
  double wire_res, wire_cap;
  resizer_->wireSignalRC(corner_, wire_res, wire_cap);

  BufferedNetPtr left = bnet->ref();
  int wire_length_left = 0;
  PinSeq loads_left;
  repairNet(left, level+1, wire_length_left, loads_left);
  float cap_left = left->cap();
  float fanout_left = left->fanout();
  float max_load_slew_left = left->maxLoadSlew();

  BufferedNetPtr right = bnet->ref2();
  int wire_length_right = 0;
  PinSeq loads_right;
  repairNet(right, level+1, wire_length_right, loads_right);
  float cap_right = right->cap();
  float fanout_right = right->fanout();
  float max_load_slew_right = right->maxLoadSlew();

  debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}left  l={} cap={} fanout={}",
             "", level,
             units_->distanceUnit()->asString(dbuToMeters(wire_length_left), 1),
             units_->capacitanceUnit()->asString(cap_left, 3),
             fanout_left);
  debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}right l={} cap={} fanout={}",
             "", level,
             units_->distanceUnit()->asString(dbuToMeters(wire_length_right), 1),
             units_->capacitanceUnit()->asString(cap_right, 3),
             fanout_right);

  wire_length = wire_length_left + wire_length_right;
  float wire_length1 = dbuToMeters(wire_length);
  float load_cap = cap_left + cap_right;
  float max_load_slew = min(max_load_slew_left, max_load_slew_right);
  LibertyCell *buffer_cell = resizer_->findTargetCell(resizer_->buffer_lowest_drive_,
                                                      load_cap, false);

  // Check for violations when the left/right branches are combined.
  // Add a buffer to left or right branch to stay under the max cap/length/fanout.
  bool repeater_left = false;
  bool repeater_right = false;
  float r_drvr = resizer_->driveResistance(drvr_pin_);
  float load_slew = (r_drvr + wire_length1 * wire_res)
    * load_cap * elmore_skew_factor_;
  bool load_slew_violation = load_slew > max_load_slew;
  // Driver slew checks were converted to max cap.
  if (load_slew_violation) {
    debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}load slew violation {} > {}",
               "", level,
               delayAsString(load_slew, this, 3),
               delayAsString(max_load_slew, this, 3));
    float slew_slack_left = max_load_slew_left
      - (r_drvr + dbuToMeters(wire_length_left) * wire_res)
      * cap_left * elmore_skew_factor_;
    float slew_slack_right = max_load_slew_right
      - (r_drvr + dbuToMeters(wire_length_right) * wire_res)
      * cap_right * elmore_skew_factor_;
    debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s} slew slack left={} right={}",
               "", level,
               delayAsString(slew_slack_left, this, 3),
               delayAsString(slew_slack_right, this, 3));
    // Isolate the branch with the smaller slack by buffering the OTHER branch.
    if (slew_slack_left < slew_slack_right)
      repeater_left = true;
    else
      repeater_right = true;
  }
  bool cap_violation = (cap_left + cap_right) > max_cap_;
  if (cap_violation) {
    debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}cap violation", "", level);
    if (cap_left > cap_right)
      repeater_left = true;
    else
      repeater_right = true;
  }
  bool length_violation = max_length_ > 0
    && (wire_length_left + wire_length_right) > max_length_;
  if (length_violation) {
    debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}length violation", "", level);
    if (wire_length_left > wire_length_right)
      repeater_left = true;
    else
      repeater_right = true;
  }
  bool fanout_violation = max_fanout_ > 0
    // Note that if both fanout_left==max_fanout and fanout_right==max_fanout
    // there is no way repair the violation (adding a buffer to either branch
    // results in max_fanout+1, which is a violation).
    // Leave room for one buffer on the other branch by using >= to avoid
    // this situation.
    && (fanout_left + fanout_right) >= max_fanout_;
  if (fanout_violation) {
    debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}fanout violation", "", level);
    if (fanout_left > fanout_right)
      repeater_left = true;
    else
      repeater_right = true;
  }

  if (repeater_left) {
    makeRepeater("left", loc, buffer_cell, true, level,
                 loads_left, cap_left, fanout_left, max_load_slew_left);
    wire_length_left = 0;
  }
  if (repeater_right) {
    makeRepeater("right", loc, buffer_cell, true, level,
                 loads_right, cap_right, fanout_right, max_load_slew_right);
    wire_length_right = 0;
  }

  // Update after left/right repeaters are inserted.
  wire_length = wire_length_left + wire_length_right;

  bnet->setCapacitance(cap_left + cap_right);
  bnet->setFanout(fanout_right + fanout_left);
  bnet->setMaxLoadSlew(min(max_load_slew_left, max_load_slew_right));

  // Union left/right load pins.
  for (Pin *load_pin : loads_left)
    load_pins.push_back(load_pin);
  for (Pin *load_pin : loads_right)
    load_pins.push_back(load_pin);
}

void
RepairDesign::repairNetLoad(BufferedNetPtr bnet,
                            int level,
                            // Return values.
                            // Remaining parasiics after repeater insertion.
                            int &wire_length, // dbu
                            PinSeq &load_pins)
{
  debugPrint(logger_, RSZ, "repair_net", 3, "{:{}s}{}",
                 "", level,
             bnet->to_string(resizer_));
  Pin *load_pin = bnet->loadPin();
  debugPrint(logger_, RSZ, "repair_net", 2, "{:{}s}load {}",
             "", level,
             sdc_network_->pathName(load_pin));
  load_pins.push_back(load_pin);
}

void
RepairDesign::makeRepeater(const char *where,
                           Point loc,
                           LibertyCell *buffer_cell,
                           bool resize,
                           int level,
                           // Return values.
                           PinSeq &load_pins,
                           float &repeater_cap,
                           float &repeater_fanout,
                           float &repeater_max_slew)
{
  makeRepeater(where, loc.getX(), loc.getY(), buffer_cell, resize,
               level, load_pins, repeater_cap, repeater_fanout,
               repeater_max_slew);
}

void
RepairDesign::makeRepeater(const char *where,
                           int x,
                           int y,
                           LibertyCell *buffer_cell,
                           bool resize,
                           int level,
                           // Return values.
                           PinSeq &load_pins,
                           float &repeater_cap,
                           float &repeater_fanout,
                           float &repeater_max_slew)
{
  LibertyPort *buffer_input_port, *buffer_output_port;
  buffer_cell->bufferPorts(buffer_input_port, buffer_output_port);
  string buffer_name = resizer_->makeUniqueInstName("repeater");
  debugPrint(logger_, RSZ, "repair_net", 2, "{:{}s}{} {} {} ({} {})",
             "", level,
             where,
             buffer_name.c_str(),
             buffer_cell->name(),
             units_->distanceUnit()->asString(dbuToMeters(x), 1),
             units_->distanceUnit()->asString(dbuToMeters(y), 1));

  // Inserting a buffer is complicated by the fact that verilog netlists
  // use the net name for input and output ports. This means the ports
  // cannot be moved to a different net.

  // This cannot depend on the net in caller because the buffer may be inserted
  // between the driver and the loads changing the net as the repair works its
  // way from the loads to the driver.

  Net *net = nullptr, *in_net, *out_net;
  bool have_output_port_load = false;
  for (Pin *pin : load_pins) {
    if (network_->isTopLevelPort(pin)) {
      net = network_->net(network_->term(pin));
      if (network_->direction(pin)->isAnyOutput()) {
        have_output_port_load = true;
        break;
      }
    }
    else
      net = network_->net(pin);
  }
  Instance *parent = db_network_->topInstance();

  // If the net is driven by an input port,
  // use the net as the repeater input net so the port stays connected to it.
  if (hasInputPort(net)
      || !have_output_port_load) {
    in_net = net;
    out_net = resizer_->makeUniqueNet();
    // Copy signal type to new net.
    dbNet *out_net_db = db_network_->staToDb(out_net);
    dbNet *in_net_db = db_network_->staToDb(in_net);
    out_net_db->setSigType(in_net_db->getSigType());

    // Move load pins to out_net.
    for (Pin *pin : load_pins) {
      Port *port = network_->port(pin);
      Instance *inst = network_->instance(pin);
      sta_->disconnectPin(pin);
      sta_->connectPin(inst, port, out_net);
    }
  }
  else {
    // One of the loads is an output port.
    // Use the net as the repeater output net so the port stays connected to it.
    in_net = resizer_->makeUniqueNet();
    out_net = net;
    // Copy signal type to new net.
    dbNet *out_net_db = db_network_->staToDb(out_net);
    dbNet *in_net_db = db_network_->staToDb(in_net);
    in_net_db->setSigType(out_net_db->getSigType());

    // Move non-repeater load pins to in_net.
    PinSet load_pins1;
    for (Pin *pin : load_pins)
      load_pins1.insert(pin);

    NetPinIterator *pin_iter = network_->pinIterator(out_net);
    while (pin_iter->hasNext()) {
      Pin *pin = pin_iter->next();
      if (!load_pins1.hasKey(pin)) {
        Port *port = network_->port(pin);
        Instance *inst = network_->instance(pin);
        sta_->disconnectPin(pin);
        sta_->connectPin(inst, port, in_net);
      }
    }
  }

  Point buf_loc(x, y);
  Instance *buffer = resizer_->makeBuffer(buffer_cell,
                                          buffer_name.c_str(),
                                          parent, buf_loc);
  inserted_buffer_count_++;

  sta_->connectPin(buffer, buffer_input_port, in_net);
  sta_->connectPin(buffer, buffer_output_port, out_net);

  resizer_->parasiticsInvalid(in_net);
  resizer_->parasiticsInvalid(out_net);

  // Resize repeater as we back up by levels.
  if (resize) {
    Pin *drvr_pin = network_->findPin(buffer, buffer_output_port);
    resizer_->resizeToTargetSlew(drvr_pin);
    buffer_cell = network_->libertyCell(buffer);
    buffer_cell->bufferPorts(buffer_input_port, buffer_output_port);
  }

  Pin *buf_in_pin = network_->findPin(buffer, buffer_input_port);
  load_pins.clear();
  load_pins.push_back(buf_in_pin);
  repeater_cap = resizer_->portCapacitance(buffer_input_port, corner_);
  repeater_fanout = resizer_->portFanoutLoad(buffer_input_port);
  repeater_max_slew = bufferInputMaxSlew(buffer_cell, corner_);
}

bool
RepairDesign::hasInputPort(const Net *net)
{
  bool has_top_level_port = false;
  NetConnectedPinIterator *pin_iter = network_->connectedPinIterator(net);
  while (pin_iter->hasNext()) {
    Pin *pin = pin_iter->next();
    if (network_->isTopLevelPort(pin)
        && network_->direction(pin)->isAnyInput()) {
      has_top_level_port = true;
      break;
    }
  }
  delete pin_iter;
  return has_top_level_port;
}

LibertyCell *
RepairDesign::findBufferUnderSlew(float max_slew,
                                  float load_cap)
{
  LibertyCell *min_slew_buffer = resizer_->buffer_lowest_drive_;
  float min_slew = INF;
  LibertyCellSeq *equiv_cells = sta_->equivCells(resizer_->buffer_lowest_drive_);
  if (equiv_cells) {
    sort(equiv_cells, [this] (const LibertyCell *buffer1,
                              const LibertyCell *buffer2) {
      return resizer_->bufferDriveResistance(buffer1)
        > resizer_->bufferDriveResistance(buffer2);
    });
    for (LibertyCell *buffer : *equiv_cells) {
      if (!resizer_->dontUse(buffer)
          && resizer_->isLinkCell(buffer)) {
        float slew = bufferSlew(buffer, load_cap,
                                resizer_->tgt_slew_dcalc_ap_);
        debugPrint(logger_, RSZ, "buffer_under_slew", 1, "{:{}s}pt ({} {})",
                   buffer->name(),
                   units_->timeUnit()->asString(slew));
        if (slew < max_slew) {
          return buffer;
        }
        if (slew < min_slew) {
          min_slew_buffer = buffer;
          min_slew = slew;
        }
      }
    }
  }
  // Could not find a buffer under max_slew but this is min slew achievable.
  return min_slew_buffer;
}

float
RepairDesign::bufferSlew(LibertyCell *buffer_cell,
                         float load_cap,
                         const DcalcAnalysisPt *dcalc_ap)
{
  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);
  ArcDelay gate_delays[RiseFall::index_count];
  Slew slews[RiseFall::index_count];
  resizer_->gateDelays(output, load_cap, dcalc_ap, gate_delays, slews);
  return max(slews[RiseFall::riseIndex()],
             slews[RiseFall::fallIndex()]);
}

double
RepairDesign::dbuToMeters(int dist) const
{
  return dist / (dbu_ * 1e+6);
}

int
RepairDesign::metersToDbu(double dist) const
{
  return dist * dbu_ * 1e+6;
}

} // namespace
