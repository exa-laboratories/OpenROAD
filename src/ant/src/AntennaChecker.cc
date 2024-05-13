// BSD 3-Clause License
//
// Copyright (c) 2020, MICL, DD-Lab, University of Michigan
// All rights reserved.
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

#include "ant/AntennaChecker.hh"

#include <tcl.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unordered_set>

#include <unordered_map>
#include <queue>

#include <boost/polygon/polygon.hpp>
#include <boost/functional/hash.hpp>

#include "odb/db.h"
#include "odb/dbTypes.h"
#include "odb/dbWireGraph.h"
#include "odb/dbShape.h"
#include "odb/wOrder.h"
#include "sta/StaMain.hh"
#include "utl/Logger.h"

namespace ant {

using odb::dbBox;
using odb::dbIoType;
using odb::dbITerm;
using odb::dbITermObj;
using odb::dbMTerm;
using odb::dbNet;
using odb::dbTechAntennaPinModel;
using odb::dbTechLayer;
using odb::dbTechLayerAntennaRule;
using odb::dbTechLayerType;
using odb::dbTechVia;
using odb::dbVia;
using odb::dbWire;
using odb::dbWireGraph;
using odb::dbWireType;
using odb::uint;
using odb::dbWirePath;
using odb::dbWirePathShape;
using odb::dbWirePathItr;

using utl::ANT;

using std::unordered_set;

// Abbreviations Index:
//   `PAR`: Partial Area Ratio
//   `CAR`: Cumulative Area Ratio
//   `Area`: Gate Area
//   `S. Area`: Side Diffusion Area
//   `C. Area`: Cumulative Gate Area
//   `C. S. Area`: Cumulative Side (Diffusion) Area

struct PARinfo
{
  odb::dbWireGraph::Node* wire_root = nullptr;
  std::set<odb::dbITerm*> iterms;
  double wire_area = 0.0;
  double side_wire_area = 0.0;
  double iterm_gate_area = 0.0;
  double iterm_diff_area = 0.0;
  double PAR = 0.0;
  double PSR = 0.0;
  double diff_PAR = 0.0;
  double diff_PSR = 0.0;
  double max_wire_length_PAR = 0.0;
  double max_wire_length_PSR = 0.0;
  double max_wire_length_diff_PAR = 0.0;
  double max_wire_length_diff_PSR = 0.0;
  double wire_length = 0.0;
  double side_wire_length = 0.0;
};

struct ARinfo
{
  PARinfo par_info;
  odb::dbWireGraph::Node* GateNode;
  double CAR;
  double CSR;
  double diff_CAR;
  double diff_CSR;
};

struct AntennaModel
{
  odb::dbTechLayer* layer;

  double metal_factor;
  double diff_metal_factor;

  double cut_factor;
  double diff_cut_factor;

  double side_metal_factor;
  double diff_side_metal_factor;

  double minus_diff_factor;
  double plus_diff_factor;
  double diff_metal_reduce_factor;
};

extern "C" {
extern int Ant_Init(Tcl_Interp* interp);
}

AntennaChecker::AntennaChecker() = default;
AntennaChecker::~AntennaChecker() = default;

void AntennaChecker::init(odb::dbDatabase* db,
                          GlobalRouteSource* global_route_source,
                          Logger* logger)
{
  db_ = db;
  global_route_source_ = global_route_source;
  logger_ = logger;
}

void AntennaChecker::initAntennaRules()
{
  block_ = db_->getChip()->getBlock();
  odb::dbTech* tech = db_->getTech();
  for (odb::dbTechLayer* tech_layer : tech->getLayers()) {
    double metal_factor = 1.0;
    double diff_metal_factor = 1.0;

    double cut_factor = 1.0;
    double diff_cut_factor = 1.0;

    double side_metal_factor = 1.0;
    double diff_side_metal_factor = 1.0;

    double minus_diff_factor = 0.0;
    double plus_diff_factor = 0.0;
    double diff_metal_reduce_factor = 1.0;

    if (tech_layer->hasDefaultAntennaRule()) {
      const dbTechLayerAntennaRule* antenna_rule
          = tech_layer->getDefaultAntennaRule();

      if (antenna_rule->isAreaFactorDiffUseOnly()) {
        diff_metal_factor = antenna_rule->getAreaFactor();

        diff_cut_factor = antenna_rule->getAreaFactor();
      } else {
        metal_factor = antenna_rule->getAreaFactor();
        diff_metal_factor = antenna_rule->getAreaFactor();

        cut_factor = antenna_rule->getAreaFactor();
        diff_cut_factor = antenna_rule->getAreaFactor();
      }
      if (antenna_rule->isSideAreaFactorDiffUseOnly()) {
        diff_side_metal_factor = antenna_rule->getSideAreaFactor();
      } else {
        side_metal_factor = antenna_rule->getSideAreaFactor();
        diff_side_metal_factor = antenna_rule->getSideAreaFactor();
      }

      minus_diff_factor = antenna_rule->getAreaMinusDiffFactor();
      plus_diff_factor = antenna_rule->getGatePlusDiffFactor();

      const double PSR_ratio = antenna_rule->getPSR();
      const dbTechLayerAntennaRule::pwl_pair diffPSR
          = antenna_rule->getDiffPSR();

      uint wire_thickness_dbu = 0;
      tech_layer->getThickness(wire_thickness_dbu);

      const dbTechLayerType layerType = tech_layer->getType();

      // If there is a SIDE area antenna rule, then make sure thickness exists.
      if ((PSR_ratio != 0 || !diffPSR.indices.empty())
          && layerType == dbTechLayerType::ROUTING && wire_thickness_dbu == 0) {
        logger_->warn(ANT,
                      13,
                      "No THICKNESS is provided for layer {}.  Checks on this "
                      "layer will not be correct.",
                      tech_layer->getConstName());
      }
    }

    AntennaModel layer_antenna = {tech_layer,
                                  metal_factor,
                                  diff_metal_factor,
                                  cut_factor,
                                  diff_cut_factor,
                                  side_metal_factor,
                                  diff_side_metal_factor,
                                  minus_diff_factor,
                                  plus_diff_factor,
                                  diff_metal_reduce_factor};
    layer_info_[tech_layer] = layer_antenna;
  }
}

dbWireGraph::Node* AntennaChecker::findSegmentRoot(dbWireGraph::Node* node,
                                                   int wire_level)
{
  if (node->in_edge() == nullptr) {
    return node;
  }

  if (node->in_edge()->type() == dbWireGraph::Edge::Type::VIA
      || node->in_edge()->type() == dbWireGraph::Edge::Type::TECH_VIA) {
    if (node->in_edge()->source()->layer()->getRoutingLevel() > wire_level) {
      return node;
    }

    dbWireGraph::Node* new_root
        = findSegmentRoot(node->in_edge()->source(), wire_level);

    if (new_root->layer()->getRoutingLevel() == wire_level) {
      return new_root;
    }
    return node;
  }

  if (node->in_edge()->type() == dbWireGraph::Edge::Type::SEGMENT
      || node->in_edge()->type() == dbWireGraph::Edge::Type::SHORT) {
    return findSegmentRoot(node->in_edge()->source(), wire_level);
  }

  return node;
}

dbWireGraph::Node* AntennaChecker::findSegmentStart(dbWireGraph::Node* node)
{
  if ((node->object() && node->object()->getObjectType() == dbITermObj)
      || !node->in_edge()) {
    return node;
  }

  if (node->in_edge()->type() == dbWireGraph::Edge::Type::VIA
      || node->in_edge()->type() == dbWireGraph::Edge::Type::TECH_VIA) {
    return node;
  }

  if (node->in_edge()->type() == dbWireGraph::Edge::Type::SEGMENT
      || node->in_edge()->type() == dbWireGraph::Edge::Type::SHORT) {
    return findSegmentStart(node->in_edge()->source());
  }

  return nullptr;
}

bool AntennaChecker::ifSegmentRoot(dbWireGraph::Node* node, int wire_level)
{
  if ((node->object() && node->object()->getObjectType() == dbITermObj)
      || !node->in_edge()) {
    return true;
  }
  if (node->in_edge()->type() == dbWireGraph::Edge::Type::VIA
      || node->in_edge()->type() == dbWireGraph::Edge::Type::TECH_VIA) {
    if (node->in_edge()->source()->layer()->getRoutingLevel() <= wire_level) {
      dbWireGraph::Node* new_root
          = findSegmentRoot(node->in_edge()->source(), wire_level);
      return new_root->layer()->getRoutingLevel() != wire_level;
    }
    return true;
  }
  return false;
}

void AntennaChecker::findWireBelowIterms(dbWireGraph::Node* node,
                                         double& iterm_gate_area,
                                         double& iterm_diff_area,
                                         int wire_level,
                                         std::set<dbITerm*>& iv,
                                         std::set<dbWireGraph::Node*>& nv)
{
  if (node->object() && node->object()->getObjectType() == dbITermObj) {
    dbITerm* iterm = dbITerm::getITerm(block_, node->object()->getId());
    if (iterm) {
      dbMTerm* mterm = iterm->getMTerm();
      iterm_gate_area += gateArea(mterm);
      iterm_diff_area += diffArea(mterm);
      iv.insert(iterm);
    }
  }

  nv.insert(node);

  if (node->in_edge()
      && node->in_edge()->source()->layer()->getRoutingLevel() <= wire_level) {
    if ((node->in_edge()->type() == dbWireGraph::Edge::Type::VIA
         || node->in_edge()->type() == dbWireGraph::Edge::Type::TECH_VIA)
        && nv.find(node->in_edge()->source()) == nv.end()) {
      findWireBelowIterms(findSegmentStart(node->in_edge()->source()),
                          iterm_gate_area,
                          iterm_diff_area,
                          wire_level,
                          iv,
                          nv);
    } else if ((node->in_edge()->type() == dbWireGraph::Edge::Type::SEGMENT
                || node->in_edge()->type() == dbWireGraph::Edge::Type::SHORT)
               && nv.find(node->in_edge()->source()) == nv.end()) {
      findWireBelowIterms(node->in_edge()->source(),
                          iterm_gate_area,
                          iterm_diff_area,
                          wire_level,
                          iv,
                          nv);
    }
  }

  dbWireGraph::Node::edge_iterator edge_itr;
  for (edge_itr = node->begin(); edge_itr != node->end(); ++edge_itr) {
    if ((*edge_itr)->type() == dbWireGraph::Edge::Type::VIA
        || (*edge_itr)->type() == dbWireGraph::Edge::Type::TECH_VIA) {
      if ((*edge_itr)->target()->layer()->getRoutingLevel() <= wire_level
          && nv.find((*edge_itr)->target()) == nv.end()) {
        findWireBelowIterms(findSegmentStart((*edge_itr)->target()),
                            iterm_gate_area,
                            iterm_diff_area,
                            wire_level,
                            iv,
                            nv);
      }
    }

    else if (((*edge_itr)->type() == dbWireGraph::Edge::Type::SEGMENT
              || (*edge_itr)->type() == dbWireGraph::Edge::Type::SHORT)
             && nv.find((*edge_itr)->target()) == nv.end()) {
      findWireBelowIterms((*edge_itr)->target(),
                          iterm_gate_area,
                          iterm_diff_area,
                          wire_level,
                          iv,
                          nv);
    }
  }
}

std::pair<double, double> AntennaChecker::calculateWireArea(
    dbWireGraph::Node* node,
    int wire_level,
    std::set<dbWireGraph::Node*>& nv,
    std::set<dbWireGraph::Node*>& level_nodes)
{
  double wire_area = 0;
  double side_wire_area = 0;

  double wire_width = block_->dbuToMicrons(node->layer()->getWidth());
  uint wire_thickness_dbu = 0;
  node->layer()->getThickness(wire_thickness_dbu);
  double wire_thickness = block_->dbuToMicrons(wire_thickness_dbu);

  int start_x, start_y;
  int end_x, end_y;
  node->xy(start_x, start_y);

  vector<std::pair<dbWireGraph::Edge*, dbIoType>> edge_vec;
  if (node->in_edge() != nullptr
      && nv.find(node->in_edge()->source()) == nv.end()) {
    edge_vec.emplace_back(node->in_edge(), dbIoType::INPUT);
  }

  dbWireGraph::Node::edge_iterator edge_it;

  for (edge_it = node->begin(); edge_it != node->end(); edge_it++) {
    if (nv.find((*edge_it)->source()) == nv.end()) {
      edge_vec.emplace_back(*edge_it, dbIoType::OUTPUT);
    }
  }

  nv.insert(node);

  for (const auto& edge_info : edge_vec) {
    dbWireGraph::Edge* edge = edge_info.first;
    dbIoType edge_io_type = edge_info.second;
    if (edge->type() == dbWireGraph::Edge::Type::VIA
        || edge->type() == dbWireGraph::Edge::Type::TECH_VIA) {
      if (edge_io_type == dbIoType::INPUT) {
        if (edge->source()->layer()->getRoutingLevel() <= wire_level) {
          std::pair<double, double> areas
              = calculateWireArea(edge->source(), wire_level, nv, level_nodes);
          wire_area += areas.first;
          side_wire_area += areas.second;
        }
      }

      if (edge_io_type == dbIoType::OUTPUT) {
        if (edge->target()->layer()->getRoutingLevel() <= wire_level) {
          std::pair<double, double> areas
              = calculateWireArea(edge->target(), wire_level, nv, level_nodes);
          wire_area += areas.first;
          side_wire_area += areas.second;
        }
      }
    }

    if (edge->type() == dbWireGraph::Edge::Type::SEGMENT
        || edge->type() == dbWireGraph::Edge::Type::SHORT) {
      if (edge_io_type == dbIoType::INPUT) {
        if (node->layer()->getRoutingLevel() == wire_level) {
          level_nodes.insert(node);
          edge->source()->xy(end_x, end_y);

          wire_area += block_->dbuToMicrons(abs(end_x - start_x)
                                            + abs(end_y - start_y))
                       * wire_width;
          side_wire_area += (block_->dbuToMicrons(abs(end_x - start_x)
                                                  + abs(end_y - start_y)))
                            * wire_thickness * 2;

          // These are added to represent the extensions to the wire segments
          // (0.5 * wire_width)
          wire_area += wire_width * wire_width;
          side_wire_area += 2 * wire_thickness * wire_width;
        }

        std::pair<double, double> areas
            = calculateWireArea(edge->source(), wire_level, nv, level_nodes);
        wire_area += areas.first;
        side_wire_area += areas.second;
      }

      if (edge_io_type == dbIoType::OUTPUT) {
        if (node->layer()->getRoutingLevel() == wire_level) {
          level_nodes.insert(node);
          edge->target()->xy(end_x, end_y);
          wire_area += block_->dbuToMicrons(abs(end_x - start_x)
                                            + abs(end_y - start_y))
                           * wire_width
                       + wire_width * wire_width;
          side_wire_area += (block_->dbuToMicrons(abs(end_x - start_x)
                                                  + abs(end_y - start_y))
                             + wire_width)
                                * wire_thickness * 2
                            + 2 * wire_thickness * wire_width;
        }

        std::pair<double, double> areas
            = calculateWireArea(edge->target(), wire_level, nv, level_nodes);
        wire_area += areas.first;
        side_wire_area += areas.second;
      }
    }
  }
  return {wire_area, side_wire_area};
}

double AntennaChecker::getViaArea(dbWireGraph::Edge* edge)
{
  double via_area = 0.0;
  if (edge->type() == dbWireGraph::Edge::Type::TECH_VIA) {
    auto tech_via_edge = (dbWireGraph::TechVia*) edge;
    dbTechVia* tech_via = tech_via_edge->via();
    for (dbBox* box : tech_via->getBoxes()) {
      if (box->getTechLayer()->getType() == dbTechLayerType::CUT) {
        uint dx = box->getDX();
        uint dy = box->getDY();
        via_area = block_->dbuToMicrons(dx) * block_->dbuToMicrons(dy);
      }
    }
  } else if (edge->type() == dbWireGraph::Edge::Type::VIA) {
    auto via_edge = (dbWireGraph::Via*) edge;
    dbVia* via = via_edge->via();
    for (dbBox* box : via->getBoxes()) {
      if (box->getTechLayer()->getType() == dbTechLayerType::CUT) {
        uint dx = box->getDX();
        uint dy = box->getDY();
        via_area = block_->dbuToMicrons(dx) * block_->dbuToMicrons(dy);
      }
    }
  }
  return via_area;
}

dbTechLayer* AntennaChecker::getViaLayer(dbWireGraph::Edge* edge)
{
  if (edge->type() == dbWireGraph::Edge::Type::TECH_VIA) {
    auto tech_via_edge = (dbWireGraph::TechVia*) edge;
    dbTechVia* tech_via = tech_via_edge->via();
    for (dbBox* box : tech_via->getBoxes()) {
      if (box->getTechLayer()->getType() == dbTechLayerType::CUT) {
        return box->getTechLayer();
      }
    }
  } else if (edge->type() == dbWireGraph::Edge::Type::VIA) {
    auto via_edge = (dbWireGraph::Via*) edge;
    dbVia* via = via_edge->via();
    for (dbBox* box : via->getBoxes()) {
      if (box->getTechLayer()->getType() == dbTechLayerType::CUT) {
        return box->getTechLayer();
      }
    }
  }
  return nullptr;
}

std::string AntennaChecker::getViaName(dbWireGraph::Edge* edge)
{
  if (edge->type() == dbWireGraph::Edge::Type::TECH_VIA) {
    auto tech_via_edge = (dbWireGraph::TechVia*) edge;
    dbTechVia* tech_via = tech_via_edge->via();
    return tech_via->getName();
  }
  if (edge->type() == dbWireGraph::Edge::Type::VIA) {
    auto via_edge = (dbWireGraph::Via*) edge;
    dbVia* via = via_edge->via();
    return via->getName();
  }
  return nullptr;
}

double AntennaChecker::calculateViaArea(dbWireGraph::Node* node, int wire_level)
{
  double via_area = 0.0;
  if (node->in_edge()
      && (node->in_edge()->type() == dbWireGraph::Edge::Type::VIA
          || node->in_edge()->type() == dbWireGraph::Edge::Type::TECH_VIA)) {
    if (node->in_edge()->source()->layer()->getRoutingLevel() > wire_level) {
      via_area = via_area + getViaArea(node->in_edge());
    }
  }

  dbWireGraph::Node::edge_iterator edge_itr;
  for (edge_itr = node->begin(); edge_itr != node->end(); ++edge_itr) {
    if ((*edge_itr)->type() == dbWireGraph::Edge::Type::SEGMENT
        || (*edge_itr)->type() == dbWireGraph::Edge::Type::SHORT) {
      via_area = via_area + calculateViaArea((*edge_itr)->target(), wire_level);
    } else if ((*edge_itr)->type() == dbWireGraph::Edge::Type::VIA
               || (*edge_itr)->type() == dbWireGraph::Edge::Type::TECH_VIA) {
      if ((*edge_itr)->target()->layer()->getRoutingLevel() > wire_level) {
        via_area = via_area + getViaArea((*edge_itr));
      } else {
        via_area
            = via_area + calculateViaArea((*edge_itr)->target(), wire_level);
      }
    }
  }
  return via_area;
}

dbWireGraph::Edge* AntennaChecker::findVia(dbWireGraph::Node* node,
                                           int wire_level)
{
  if (node->in_edge()
      && (node->in_edge()->type() == dbWireGraph::Edge::Type::VIA
          || node->in_edge()->type() == dbWireGraph::Edge::Type::TECH_VIA)) {
    if (node->in_edge()->source()->layer()->getRoutingLevel() > wire_level) {
      return node->in_edge();
    }
  }
  dbWireGraph::Node::edge_iterator edge_itr;
  for (edge_itr = node->begin(); edge_itr != node->end(); ++edge_itr) {
    if ((*edge_itr)->type() == dbWireGraph::Edge::Type::SEGMENT
        || (*edge_itr)->type() == dbWireGraph::Edge::Type::SHORT) {
      dbWireGraph::Edge* via = findVia((*edge_itr)->target(), wire_level);
      if (via) {
        return via;
      }
    } else if ((*edge_itr)->type() == dbWireGraph::Edge::Type::VIA
               || (*edge_itr)->type() == dbWireGraph::Edge::Type::TECH_VIA) {
      if ((*edge_itr)->target()->layer()->getRoutingLevel() > wire_level) {
        return (*edge_itr);
      }
      dbWireGraph::Edge* via = findVia((*edge_itr)->target(), wire_level);
      if (via) {
        return via;
      }
    }
  }
  return nullptr;
}

void AntennaChecker::findCarPath(dbWireGraph::Node* node,
                                 int wire_level,
                                 dbWireGraph::Node* goal,
                                 vector<dbWireGraph::Node*>& current_path,
                                 vector<dbWireGraph::Node*>& path_found)
{
  current_path.push_back(node);

  if (node == goal) {
    for (dbWireGraph::Node* node : current_path) {
      bool node_exists = false;
      for (dbWireGraph::Node* found_node : path_found) {
        if (node == found_node) {
          node_exists = true;
          break;
        }
      }
      if (!node_exists) {
        path_found.push_back(node);
      }
    }
  } else {
    if (node->in_edge()
        && (node->in_edge()->type() == dbWireGraph::Edge::Type::VIA
            || node->in_edge()->type() == dbWireGraph::Edge::Type::TECH_VIA)) {
      if (node->in_edge()->source()->layer()->getRoutingLevel()
          < node->in_edge()->target()->layer()->getRoutingLevel()) {
        auto root_info = findSegmentRoot(
            node->in_edge()->source(),
            node->in_edge()->source()->layer()->getRoutingLevel());
        findCarPath(root_info,
                    node->in_edge()->source()->layer()->getRoutingLevel(),
                    goal,
                    current_path,
                    path_found);
      }
    }
    dbWireGraph::Node::edge_iterator edge_itr;
    for (edge_itr = node->begin(); edge_itr != node->end(); ++edge_itr) {
      if ((*edge_itr)->type() == dbWireGraph::Edge::Type::VIA
          || (*edge_itr)->type() == dbWireGraph::Edge::Type::TECH_VIA) {
        if ((*edge_itr)->target()->layer()->getRoutingLevel() <= wire_level) {
          findCarPath(findSegmentStart((*edge_itr)->target()),
                      wire_level,
                      goal,
                      current_path,
                      path_found);
        }
      } else if ((*edge_itr)->type() == dbWireGraph::Edge::Type::SEGMENT
                 || (*edge_itr)->type() == dbWireGraph::Edge::Type::SHORT) {
        findCarPath(
            (*edge_itr)->target(), wire_level, goal, current_path, path_found);
      }
    }
  }
  current_path.pop_back();
}

vector<PARinfo> AntennaChecker::buildWireParTable(
    const vector<dbWireGraph::Node*>& wire_roots)
{
  vector<PARinfo> PARtable;
  std::set<dbWireGraph::Node*> level_nodes;
  for (dbWireGraph::Node* wire_root : wire_roots) {
    if (level_nodes.find(wire_root) != level_nodes.end()) {
      continue;
    }

    std::set<dbWireGraph::Node*> nv;
    std::pair<double, double> areas = calculateWireArea(
        wire_root, wire_root->layer()->getRoutingLevel(), nv, level_nodes);

    double wire_area = areas.first;
    double side_wire_area = areas.second;
    double iterm_gate_area = 0.0;
    double iterm_diff_area = 0.0;
    std::set<dbITerm*> iv;
    nv.clear();

    findWireBelowIterms(wire_root,
                        iterm_gate_area,
                        iterm_diff_area,
                        wire_root->layer()->getRoutingLevel(),
                        iv,
                        nv);

    PARinfo par_info;
    par_info.wire_root = wire_root;
    par_info.iterms = std::move(iv);
    par_info.wire_area = wire_area;
    par_info.side_wire_area = side_wire_area;
    par_info.iterm_gate_area = iterm_gate_area;
    par_info.iterm_diff_area = iterm_diff_area;
    //std::cout << "Layer: " << wire_root->layer()->getName() << " wire area: " << wire_area << " side area: " << side_wire_area << "\n";
    PARtable.push_back(par_info);
  }

  for (PARinfo& par_info : PARtable) {
    calculateParInfo(par_info);
  }

  return PARtable;
}

double AntennaChecker::gateArea(dbMTerm* mterm)
{
  double max_gate_area = 0;
  if (mterm->hasDefaultAntennaModel()) {
    dbTechAntennaPinModel* pin_model = mterm->getDefaultAntennaModel();
    vector<std::pair<double, dbTechLayer*>> gate_areas;
    pin_model->getGateArea(gate_areas);

    for (const auto& [gate_area, layer] : gate_areas) {
      max_gate_area = std::max(max_gate_area, gate_area);
    }
  }
  return max_gate_area;
}

double AntennaChecker::getPwlFactor(dbTechLayerAntennaRule::pwl_pair pwl_info,
                                    double ref_value,
                                    double default_value)
{
  if (!pwl_info.indices.empty()) {
    if (pwl_info.indices.size() == 1) {
      return pwl_info.ratios[0];
    }
    double pwl_info_index1 = pwl_info.indices[0];
    double pwl_info_ratio1 = pwl_info.ratios[0];
    double slope = 1.0;
    for (int i = 0; i < pwl_info.indices.size(); i++) {
      double pwl_info_index2 = pwl_info.indices[i];
      double pwl_info_ratio2 = pwl_info.ratios[i];
      slope = (pwl_info_ratio2 - pwl_info_ratio1)
              / (pwl_info_index2 - pwl_info_index1);

      if (ref_value >= pwl_info_index1 && ref_value < pwl_info_index2) {
        return pwl_info_ratio1 + (ref_value - pwl_info_index1) * slope;
      }
      pwl_info_index1 = pwl_info_index2;
      pwl_info_ratio1 = pwl_info_ratio2;
    }
    return pwl_info_ratio1 + (ref_value - pwl_info_index1) * slope;
  }
  return default_value;
}

void AntennaChecker::calculateParInfo(PARinfo& par_info)
{
  dbWireGraph::Node* wire_root = par_info.wire_root;
  odb::dbTechLayer* tech_layer = wire_root->layer();
  AntennaModel& am = layer_info_[tech_layer];

  double metal_factor = am.metal_factor;
  double diff_metal_factor = am.diff_metal_factor;
  double side_metal_factor = am.side_metal_factor;
  double diff_side_metal_factor = am.diff_side_metal_factor;

  double minus_diff_factor = am.minus_diff_factor;
  double plus_diff_factor = am.plus_diff_factor;

  double diff_metal_reduce_factor = am.diff_metal_reduce_factor;

  if (tech_layer->hasDefaultAntennaRule()) {
    const dbTechLayerAntennaRule* antenna_rule
        = tech_layer->getDefaultAntennaRule();
    diff_metal_reduce_factor = getPwlFactor(
        antenna_rule->getAreaDiffReduce(), par_info.iterm_diff_area, 1.0);
  }

  if (par_info.iterm_gate_area == 0 || !tech_layer->hasDefaultAntennaRule()) {
    return;
  }

  // Find the theoretical limits for PAR and its variants
  const dbTechLayerAntennaRule* antenna_rule
      = tech_layer->getDefaultAntennaRule();

  const double PAR_ratio = antenna_rule->getPAR();
  const dbTechLayerAntennaRule::pwl_pair diffPAR = antenna_rule->getDiffPAR();
  const double diffPAR_PWL_ratio
      = getPwlFactor(diffPAR, par_info.iterm_diff_area, 0);

  const double PSR_ratio = antenna_rule->getPSR();
  const dbTechLayerAntennaRule::pwl_pair diffPSR = antenna_rule->getDiffPSR();
  const double diffPSR_PWL_ratio
      = getPwlFactor(diffPSR, par_info.iterm_diff_area, 0.0);

  // Extract the width and thickness
  const double wire_width = block_->dbuToMicrons(tech_layer->getWidth());
  uint thickness;
  tech_layer->getThickness(thickness);
  const double wire_thickness = block_->dbuToMicrons(thickness);

  // Calculate the current wire length from the area taking into consideration
  // the extensions
  par_info.wire_length = par_info.wire_area / wire_width - wire_width;
  par_info.side_wire_length
      = (par_info.side_wire_area - 2 * wire_width * wire_thickness)
            / (2 * wire_thickness)
        - wire_width;

  // Consider when there is a diffusion region connected
  if (par_info.iterm_diff_area != 0) {
    // Calculate the maximum allowed wire length for each PAR variant
    const double max_area_PAR
        = PAR_ratio * par_info.iterm_gate_area / diff_metal_factor;
    par_info.max_wire_length_PAR = max_area_PAR / wire_width - wire_width;

    const double max_area_PSR
        = PSR_ratio * par_info.iterm_gate_area / diff_side_metal_factor;
    par_info.max_wire_length_PSR
        = (max_area_PSR - 2 * wire_width * wire_thickness)
              / (2 * wire_thickness)
          - wire_width;

    const double max_area_diff_PAR
        = (diffPAR_PWL_ratio
               * (par_info.iterm_gate_area
                  + plus_diff_factor * par_info.iterm_diff_area)
           + minus_diff_factor * par_info.iterm_diff_area)
          / diff_metal_factor * diff_metal_reduce_factor;
    par_info.max_wire_length_diff_PAR
        = max_area_diff_PAR / wire_width - wire_width;

    const double max_area_diff_PSR
        = (diffPSR_PWL_ratio
               * (par_info.iterm_gate_area
                  + plus_diff_factor * par_info.iterm_diff_area)
           + minus_diff_factor * par_info.iterm_diff_area)
          / diff_side_metal_factor * diff_metal_reduce_factor;
    par_info.max_wire_length_diff_PSR
        = (max_area_diff_PSR - 2 * wire_width * wire_thickness)
              / (2 * wire_thickness)
          - wire_width;

    // Calculate PAR, PSR, diff_PAR and diff_PSR
    par_info.PAR
        = (diff_metal_factor * par_info.wire_area) / par_info.iterm_gate_area;
    par_info.PSR = (diff_side_metal_factor * par_info.side_wire_area)
                   / par_info.iterm_gate_area;
    par_info.diff_PAR
        = (diff_metal_factor * par_info.wire_area * diff_metal_reduce_factor
           - minus_diff_factor * par_info.iterm_diff_area)
          / (par_info.iterm_gate_area
             + plus_diff_factor * par_info.iterm_diff_area);
    par_info.diff_PSR = (diff_side_metal_factor * par_info.side_wire_area
                             * diff_metal_reduce_factor
                         - minus_diff_factor * par_info.iterm_diff_area)
                        / (par_info.iterm_gate_area
                           + plus_diff_factor * par_info.iterm_diff_area);
  } else {
    // Calculate the maximum allowed wire length for each PAR variant
    double max_area_PAR = PAR_ratio * par_info.iterm_gate_area / metal_factor;
    par_info.max_wire_length_PAR = max_area_PAR / wire_width - wire_width;

    double max_area_PSR
        = PSR_ratio * par_info.iterm_gate_area / side_metal_factor;
    par_info.max_wire_length_PSR
        = (max_area_PSR - 2 * wire_width * wire_thickness)
              / (2 * wire_thickness)
          - wire_width;

    double max_area_diff_PAR = (diffPAR_PWL_ratio * par_info.iterm_gate_area)
                               / (diff_metal_reduce_factor * metal_factor);
    par_info.max_wire_length_diff_PAR
        = max_area_diff_PAR / wire_width - wire_width;

    double max_area_diff_PSR = (diffPSR_PWL_ratio * par_info.iterm_gate_area)
                               / (diff_metal_reduce_factor * side_metal_factor);
    par_info.max_wire_length_diff_PSR
        = (max_area_diff_PSR - 2 * wire_width * wire_thickness)
              / (2 * wire_thickness)
          - wire_width;

    // Calculate PAR, PSR, diff_PAR and diff_PSR

    par_info.PAR
        = (metal_factor * par_info.wire_area) / par_info.iterm_gate_area;
    par_info.PSR = (side_metal_factor * par_info.side_wire_area)
                   / par_info.iterm_gate_area;
    par_info.diff_PAR
        = (metal_factor * par_info.wire_area * diff_metal_reduce_factor)
          / par_info.iterm_gate_area;
    par_info.diff_PSR = (side_metal_factor * par_info.side_wire_area
                         * diff_metal_reduce_factor)
                        / (par_info.iterm_gate_area);
  }
}

vector<ARinfo> AntennaChecker::buildWireCarTable(
    const vector<PARinfo>& PARtable,
    const vector<PARinfo>& VIA_PARtable,
    const vector<dbWireGraph::Node*>& gate_iterms)
{
  vector<ARinfo> CARtable;
  for (dbWireGraph::Node* gate : gate_iterms) {
    for (const PARinfo& ar : PARtable) {
      dbWireGraph::Node* wire_root = ar.wire_root;
      double car = 0.0;
      double csr = 0.0;
      double diff_car = 0.0;
      double diff_csr = 0.0;
      vector<dbWireGraph::Node*> current_path;
      vector<dbWireGraph::Node*> path_found;
      vector<dbWireGraph::Node*> car_wire_roots;

      findCarPath(wire_root,
                  wire_root->layer()->getRoutingLevel(),
                  gate,
                  current_path,
                  path_found);
      if (!path_found.empty()) {
        for (dbWireGraph::Node* node : path_found) {
          if (ifSegmentRoot(node, node->layer()->getRoutingLevel())) {
            car_wire_roots.push_back(node);
          }
        }

        vector<dbWireGraph::Node*>::iterator car_root_itr;
        for (car_root_itr = car_wire_roots.begin();
             car_root_itr != car_wire_roots.end();
             ++car_root_itr) {
          dbWireGraph::Node* car_root = *car_root_itr;
          for (const PARinfo& par_info : PARtable) {
            if (par_info.wire_root == car_root) {
              car = car + par_info.PAR;
              csr = csr + par_info.PSR;
              diff_car += par_info.diff_PAR;
              diff_csr += par_info.diff_PSR;
              break;
            }
          }
          dbTechLayer* wire_layer = wire_root->layer();
          if (wire_layer->hasDefaultAntennaRule()) {
            const dbTechLayerAntennaRule* antenna_rule
                = wire_layer->getDefaultAntennaRule();
            if (antenna_rule->hasAntennaCumRoutingPlusCut()) {
              if (car_root->layer()->getRoutingLevel()
                  < wire_root->layer()->getRoutingLevel()) {
                for (const PARinfo& via_par_info : VIA_PARtable) {
                  if (via_par_info.wire_root == car_root) {
                    car += via_par_info.PAR;
                    diff_car += via_par_info.diff_PAR;
                    break;
                  }
                }
              }
            }
          }
        }

        ARinfo car_info = {
            ar,
            gate,
            car,
            csr,
            diff_car,
            diff_csr,
        };

        CARtable.push_back(car_info);
      }
    }
  }
  return CARtable;
}

vector<PARinfo> AntennaChecker::buildViaParTable(
    const vector<dbWireGraph::Node*>& wire_roots)
{
  vector<PARinfo> VIA_PARtable;
  for (dbWireGraph::Node* wire_root : wire_roots) {
    double via_area
        = calculateViaArea(wire_root, wire_root->layer()->getRoutingLevel());
    double iterm_gate_area = 0.0;
    double iterm_diff_area = 0.0;
    std::set<dbITerm*> iv;
    std::set<dbWireGraph::Node*> nv;
    findWireBelowIterms(wire_root,
                        iterm_gate_area,
                        iterm_diff_area,
                        wire_root->layer()->getRoutingLevel(),
                        iv,
                        nv);
    double par = 0.0;
    double diff_par = 0.0;

    double cut_factor = 1.0;
    double diff_cut_factor = 1.0;

    double minus_diff_factor = 0.0;
    double plus_diff_factor = 0.0;
    double diff_metal_reduce_factor = 1.0;

    if (via_area != 0 && iterm_gate_area != 0) {
      dbTechLayer* layer = getViaLayer(
          findVia(wire_root, wire_root->layer()->getRoutingLevel()));

      AntennaModel& am = layer_info_[layer];
      diff_metal_reduce_factor = am.diff_metal_reduce_factor;
      if (layer->hasDefaultAntennaRule()) {
        const dbTechLayerAntennaRule* antenna_rule
            = layer->getDefaultAntennaRule();
        diff_metal_reduce_factor = getPwlFactor(
            antenna_rule->getAreaDiffReduce(), iterm_diff_area, 1.0);
      }
      cut_factor = am.cut_factor;
      diff_cut_factor = am.diff_cut_factor;

      minus_diff_factor = am.minus_diff_factor;
      plus_diff_factor = am.plus_diff_factor;

      if (iterm_diff_area != 0) {
        par = (diff_cut_factor * via_area) / iterm_gate_area;
        diff_par = (diff_cut_factor * via_area * diff_metal_reduce_factor
                    - minus_diff_factor * iterm_diff_area)
                   / (iterm_gate_area + plus_diff_factor * iterm_diff_area);
      } else {
        par = (cut_factor * via_area) / iterm_gate_area;
        diff_par = (cut_factor * via_area * diff_metal_reduce_factor
                    - minus_diff_factor * iterm_diff_area)
                   / (iterm_gate_area + plus_diff_factor * iterm_diff_area);
      }

      PARinfo par_info;
      par_info.wire_root = wire_root;
      par_info.iterms = std::move(iv);
      par_info.PAR = par;
      par_info.diff_PAR = diff_par;

      VIA_PARtable.push_back(par_info);
    }
  }
  return VIA_PARtable;
}

vector<ARinfo> AntennaChecker::buildViaCarTable(
    const vector<PARinfo>& PARtable,
    const vector<PARinfo>& VIA_PARtable,
    const vector<dbWireGraph::Node*>& gate_iterms)
{
  vector<ARinfo> VIA_CARtable;
  for (dbWireGraph::Node* gate : gate_iterms) {
    int x, y;
    gate->xy(x, y);

    for (const PARinfo& ar : VIA_PARtable) {
      dbWireGraph::Node* wire_root = ar.wire_root;
      double car = 0.0;
      double diff_car = 0.0;
      vector<dbWireGraph::Node*> current_path;
      vector<dbWireGraph::Node*> path_found;
      vector<dbWireGraph::Node*> car_wire_roots;

      findCarPath(wire_root,
                  wire_root->layer()->getRoutingLevel(),
                  gate,
                  current_path,
                  path_found);
      if (!path_found.empty()) {
        for (dbWireGraph::Node* node : path_found) {
          int x, y;
          node->xy(x, y);
          if (ifSegmentRoot(node, node->layer()->getRoutingLevel())) {
            car_wire_roots.push_back(node);
          }
        }
        for (dbWireGraph::Node* car_root : car_wire_roots) {
          int x, y;
          car_root->xy(x, y);
          for (const PARinfo& via_par : VIA_PARtable) {
            if (via_par.wire_root == car_root) {
              car = car + via_par.PAR;
              diff_car = diff_car + via_par.diff_PAR;
              break;
            }
          }
          dbTechLayer* via_layer = getViaLayer(
              findVia(wire_root, wire_root->layer()->getRoutingLevel()));
          if (via_layer->hasDefaultAntennaRule()) {
            const dbTechLayerAntennaRule* antenna_rule
                = via_layer->getDefaultAntennaRule();
            if (antenna_rule->hasAntennaCumRoutingPlusCut()) {
              for (const PARinfo& par : PARtable) {
                if (par.wire_root == car_root) {
                  car += par.PAR;
                  diff_car += par.diff_PAR;
                  break;
                }
              }
            }
          }
        }

        ARinfo car_info = {ar, gate, car, 0.0, diff_car, 0.0};
        VIA_CARtable.push_back(car_info);
      }
    }
  }
  return VIA_CARtable;
}

std::pair<bool, bool> AntennaChecker::checkWirePar(const ARinfo& AntennaRatio,
                                                   bool report,
                                                   bool verbose,
                                                   std::ofstream& report_file)
{
  dbTechLayer* layer = AntennaRatio.par_info.wire_root->layer();
  const double par = AntennaRatio.par_info.PAR;
  const double psr = AntennaRatio.par_info.PSR;
  const double diff_par = AntennaRatio.par_info.diff_PAR;
  const double diff_psr = AntennaRatio.par_info.diff_PSR;
  const double diff_area = AntennaRatio.par_info.iterm_diff_area;

  bool checked = false;
  bool violated = false;

  bool par_violation = false;
  bool diff_par_violation = false;
  bool psr_violation = false;
  bool diff_psr_violation = false;

  if (layer->hasDefaultAntennaRule()) {
    const dbTechLayerAntennaRule* antenna_rule = layer->getDefaultAntennaRule();

    const double PAR_ratio = antenna_rule->getPAR();
    dbTechLayerAntennaRule::pwl_pair diffPAR = antenna_rule->getDiffPAR();
    const double diffPAR_PWL_ratio = getPwlFactor(diffPAR, diff_area, 0);

    if (PAR_ratio != 0) {
      if (par > PAR_ratio) {
        par_violation = true;
        violated = true;
      }
    } else {
      if (diffPAR_PWL_ratio != 0) {
        checked = true;
        if (diff_par > diffPAR_PWL_ratio) {
          diff_par_violation = true;
          violated = true;
        }
      }
    }

    const double PSR_ratio = antenna_rule->getPSR();
    const dbTechLayerAntennaRule::pwl_pair diffPSR = antenna_rule->getDiffPSR();
    const double diffPSR_PWL_ratio = getPwlFactor(diffPSR, diff_area, 0.0);

    if (PSR_ratio != 0) {
      if (psr > PSR_ratio) {
        psr_violation = true;
        violated = true;
      }
    } else {
      if (diffPSR_PWL_ratio != 0) {
        checked = true;
        if (diff_psr > diffPSR_PWL_ratio) {
          diff_psr_violation = true;
          violated = true;
        }
      }
    }

    if (report) {
      if (PAR_ratio != 0) {
        if (par_violation || verbose) {
          std::string par_report = fmt::format(
              "      Partial area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Gate area) {}",
              par,
              PAR_ratio,
              par_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << par_report << "\n";
          }
          if (verbose) {
            logger_->report("{}", par_report);
          }
        }
      } else {
        if (diff_par_violation || verbose) {
          std::string par_report = fmt::format(
              "      Partial area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Gate area) {}",
              diff_par,
              diffPAR_PWL_ratio,
              diff_par_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << par_report << "\n";
          }
          if (verbose) {
            logger_->report("{}", par_report);
          }
        }
      }

      if (PSR_ratio != 0) {
        if (psr_violation || verbose) {
          std::string par_report = fmt::format(
              "      Partial area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Side area) {}",
              psr,
              PSR_ratio,
              psr_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << par_report << "\n";
          }
          if (verbose) {
            logger_->report("{}", par_report);
          }
        }
      } else {
        if (diff_psr_violation || verbose) {
          std::string par_report = fmt::format(
              "      Partial area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Side area) {}",
              diff_psr,
              diffPSR_PWL_ratio,
              diff_psr_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << par_report << "\n";
          }
          if (verbose) {
            logger_->report("{}", par_report);
          }
        }
      }
    }
  }
  return {violated, checked};
}

std::pair<bool, bool> AntennaChecker::checkWireCar(const ARinfo& AntennaRatio,
                                                   bool par_checked,
                                                   bool report,
                                                   bool verbose,
                                                   std::ofstream& report_file)
{
  dbTechLayer* layer = AntennaRatio.par_info.wire_root->layer();
  const double car = AntennaRatio.CAR;
  const double csr = AntennaRatio.CSR;
  const double diff_csr = AntennaRatio.diff_CSR;
  const double diff_area = AntennaRatio.par_info.iterm_diff_area;

  bool checked = false;
  bool violated = false;

  bool car_violation = false;
  bool diff_car_violation = false;
  bool csr_violation = false;
  bool diff_csr_violation = false;

  if (layer->hasDefaultAntennaRule()) {
    const dbTechLayerAntennaRule* antenna_rule = layer->getDefaultAntennaRule();

    const double CAR_ratio = par_checked ? 0.0 : antenna_rule->getCAR();
    dbTechLayerAntennaRule::pwl_pair diffCAR = antenna_rule->getDiffCAR();
    const double diffCAR_PWL_ratio
        = par_checked ? 0.0 : getPwlFactor(diffCAR, diff_area, 0);
    if (CAR_ratio != 0) {
      if (car > CAR_ratio) {
        car_violation = true;
        violated = true;
      }
    } else {
      if (diffCAR_PWL_ratio != 0) {
        checked = true;
        if (car > diffCAR_PWL_ratio) {
          diff_car_violation = true;
          violated = true;
        }
      }
    }

    const double CSR_ratio = par_checked ? 0.0 : antenna_rule->getCSR();
    dbTechLayerAntennaRule::pwl_pair diffCSR = antenna_rule->getDiffCSR();
    const double diffCSR_PWL_ratio
        = par_checked ? 0.0 : getPwlFactor(diffCSR, diff_area, 0.0);
    if (CSR_ratio != 0) {
      if (csr > CSR_ratio) {
        csr_violation = true;
        violated = true;
      }
    } else {
      if (diffCSR_PWL_ratio != 0) {
        checked = true;
        if (diff_csr > diffCSR_PWL_ratio) {
          diff_csr_violation = true;
          violated = true;
        }
      }
    }

    if (report) {
      if (CAR_ratio != 0) {
        if (car_violation || verbose) {
          std::string car_report = fmt::format(
              "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Cumulative area) {}",
              car,
              CAR_ratio,
              car_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << car_report << "\n";
          }
          logger_->report("{}", car_report);
        }
      } else {
        if (diff_car_violation || verbose) {
          std::string car_report = fmt::format(
              "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Cumulative area) {}",
              car,
              diffCAR_PWL_ratio,
              diff_car_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << car_report << "\n";
          }
          logger_->report("{}", car_report);
        }
      }

      if (CSR_ratio != 0) {
        if (car_violation || verbose) {
          std::string car_report = fmt::format(
              "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Cumulative side area) {}",
              csr,
              CSR_ratio,
              csr_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << car_report << "\n";
          }
          logger_->report("{}", car_report);
        }
      } else {
        if (diff_car_violation || verbose) {
          std::string car_report = fmt::format(
              "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Cumulative side area) {}",
              diff_csr,
              diffCSR_PWL_ratio,
              diff_csr_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << car_report << "\n";
          }
          logger_->report("{}", car_report);
        }
      }
    }
  }
  return {violated, checked};
}

bool AntennaChecker::checkViaPar(const ARinfo& AntennaRatio,
                                 bool report,
                                 bool verbose,
                                 std::ofstream& report_file)
{
  const dbTechLayer* layer = getViaLayer(
      findVia(AntennaRatio.par_info.wire_root,
              AntennaRatio.par_info.wire_root->layer()->getRoutingLevel()));
  const double par = AntennaRatio.par_info.PAR;
  const double diff_par = AntennaRatio.par_info.diff_PAR;
  const double diff_area = AntennaRatio.par_info.iterm_diff_area;

  bool violated = false;
  bool par_violation = false;
  bool diff_par_violation = false;

  if (layer->hasDefaultAntennaRule()) {
    const dbTechLayerAntennaRule* antenna_rule = layer->getDefaultAntennaRule();
    const double PAR_ratio = antenna_rule->getPAR();

    dbTechLayerAntennaRule::pwl_pair diffPAR = antenna_rule->getDiffPAR();
    const double diffPAR_PWL_ratio = getPwlFactor(diffPAR, diff_area, 0);
    if (PAR_ratio != 0) {
      if (par > PAR_ratio) {
        par_violation = true;
        violated = true;
      }
    } else {
      if (diffPAR_PWL_ratio != 0) {
        if (diff_par > diffPAR_PWL_ratio) {
          diff_par_violation = true;
          violated = true;
        }
      }
    }

    if (report) {
      if (PAR_ratio != 0) {
        if (par_violation || verbose) {
          std::string par_report = fmt::format(
              "      Partial area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Gate area) {}",
              par,
              PAR_ratio,
              par_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << par_report << "\n";
          }
          logger_->report("{}", par_report);
        }
      } else {
        if (diff_par_violation || verbose) {
          std::string par_report = fmt::format(
              "      Partial area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Gate area) {}",
              par,
              diffPAR_PWL_ratio,
              diff_par_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << par_report << "\n";
          }
          logger_->report("{}", par_report);
        }
      }
    }
  }

  return violated;
}

bool AntennaChecker::checkViaCar(const ARinfo& AntennaRatio,
                                 bool report,
                                 bool verbose,
                                 std::ofstream& report_file)
{
  dbTechLayer* layer = getViaLayer(
      findVia(AntennaRatio.par_info.wire_root,
              AntennaRatio.par_info.wire_root->layer()->getRoutingLevel()));
  const double car = AntennaRatio.CAR;
  const double diff_area = AntennaRatio.par_info.iterm_diff_area;

  bool violated = false;

  bool car_violation = false;
  bool diff_car_violation = false;

  if (layer->hasDefaultAntennaRule()) {
    const dbTechLayerAntennaRule* antenna_rule = layer->getDefaultAntennaRule();
    const double CAR_ratio = antenna_rule->getCAR();

    dbTechLayerAntennaRule::pwl_pair diffCAR = antenna_rule->getDiffCAR();
    const double diffCAR_PWL_ratio = getPwlFactor(diffCAR, diff_area, 0);

    if (CAR_ratio != 0) {
      if (car > CAR_ratio) {
        car_violation = true;
        violated = true;
      }
    } else {
      if (diffCAR_PWL_ratio != 0) {
        if (car > diffCAR_PWL_ratio) {
          diff_car_violation = true;
          violated = true;
        }
      }
    }

    if (report) {
      if (CAR_ratio != 0) {
        if (car_violation || verbose) {
          std::string car_report = fmt::format(
              "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Cumulative area) {}",
              car,
              CAR_ratio,
              car_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << car_report << "\n";
          }
          logger_->report("{}", car_report);
        }
      } else {
        if (diff_car_violation || verbose) {
          std::string car_report = fmt::format(
              "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
              "{:7.2f} "
              "(Cumulative area) {}",
              car,
              diffCAR_PWL_ratio,
              diff_car_violation ? "(VIOLATED)" : "");

          if (report_file.is_open()) {
            report_file << car_report << "\n";
          }
          logger_->report("{}", car_report);
        }
      }
    }
  }
  return violated;
}

vector<dbWireGraph::Node*> AntennaChecker::findWireRoots(dbWire* wire)
{
  vector<dbWireGraph::Node*> wire_roots;
  vector<dbWireGraph::Node*> gate_iterms;
  findWireRoots(wire, wire_roots, gate_iterms);
  return wire_roots;
}

void AntennaChecker::findWireRoots(dbWire* wire,
                                   // Return values.
                                   vector<dbWireGraph::Node*>& wire_roots,
                                   vector<dbWireGraph::Node*>& gate_iterms)
{
  dbWireGraph graph;
  graph.decode(wire);
  dbWireGraph::node_iterator node_itr;
  for (node_itr = graph.begin_nodes(); node_itr != graph.end_nodes();
       ++node_itr) {
    dbWireGraph::Node* node = *node_itr;

    auto wire_root_info
        = findSegmentRoot(node, node->layer()->getRoutingLevel());
    dbWireGraph::Node* wire_root = wire_root_info;

    if (wire_root) {
      bool found_root = false;
      for (dbWireGraph::Node* root : wire_roots) {
        if (found_root) {
          break;
        }
        if (root == wire_root) {
          found_root = true;
        }
      }
      if (!found_root) {
        wire_roots.push_back(wire_root_info);
      }
    }
    if (node->object() && node->object()->getObjectType() == dbITermObj) {
      dbITerm* iterm = dbITerm::getITerm(block_, node->object()->getId());
      dbMTerm* mterm = iterm->getMTerm();
      if (mterm->getIoType() == dbIoType::INPUT && gateArea(mterm) > 0.0) {
        gate_iterms.push_back(node);
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////////////////////

namespace gtl = boost::polygon;
using namespace gtl::operators;

using Polygon =  gtl::polygon_90_data<int>;
using PolygonSet = std::vector<Polygon>;
using Point = gtl::polygon_traits<Polygon>::point_type;

struct PinType {
  bool isITerm;
  std::string name;
  union {
    odb::dbITerm* iterm;
    odb::dbBTerm* bterm;
  };
  PinType(std::string name_, odb::dbITerm* iterm_){name=name_;iterm = iterm_;isITerm = true;}
  PinType(std::string name_, odb::dbBTerm* bterm_){name=name_;bterm = bterm_;isITerm = false;}
  bool operator==(const PinType& t) const{
      return (this->name == t.name);
   }
};

class PinTypeHash {
   public:
      size_t operator()(const PinType& t) const{
         return std::hash<std::string>{}(t.name);
   }
};

struct GraphNode {
  int id;
  bool isVia;
  Polygon pol;
  std::vector<int> low_adj;
  std::unordered_set<PinType, PinTypeHash> gates;
  GraphNode() {}
  GraphNode(int id_, bool isVia_, Polygon pol_) {id = id_; isVia = isVia_; pol = pol_;}
};

Polygon rectToPolygon(const odb::Rect& rect) {
  std::vector<Point> points{{gtl::construct<Point>(rect.xMin(),rect.yMin()),
                             gtl::construct<Point>(rect.xMin(),rect.yMax()),
                             gtl::construct<Point>(rect.xMax(),rect.yMax()),
                             gtl::construct<Point>(rect.xMax(),rect.yMin())}};
  Polygon pol;
  gtl::set_points(pol, points.begin(), points.end());
  return pol;
}

// used to find the indeces of the elements which intersect with the element pol
std::vector<int> findNodesWithIntersection(const GraphNodeVector& graph_nodes, const Polygon& pol) {
  PolygonSet objs;
  objs += pol;
  int last_size = 1;
  int index = 0;
  std::vector<int> ids;
  for (const auto& node : graph_nodes) {
    objs += node->pol;
    if (last_size == objs.size()) {
      ids.push_back(index);
    }
    index++;
    last_size = objs.size();
  }
  return ids;
}

// DSU functions
void AntennaChecker::init_dsu() {
  dsu_parent_.resize(node_count_);
  dsu_size_.resize(node_count_);
  for (int i = 0; i < node_count_; i++) {
    dsu_size_[i] = 1;
    dsu_parent_[i] = i;
  }
}

int AntennaChecker::find_set(int u) {
  if (u == dsu_parent_[u]) {
    return u;
  }
  return dsu_parent_[u] = find_set(dsu_parent_[u]);
}

bool AntennaChecker::dsu_same(int u, int v) {
  return find_set(u) == find_set(v);
}

void AntennaChecker::union_set(int u, int v) {
  u = find_set(u);
  v = find_set(v);
  // union the smaller set to bigger set
  if (dsu_size_[u] < dsu_size_[v]) {
    std::swap(u,v);
  }
  dsu_parent_[v] = u;
  dsu_size_[u] += dsu_size_[v];
}

void AntennaChecker::saveGates (dbNet* db_net){

  std::unordered_map<PinType, std::vector<int>, PinTypeHash> pin_nbrs;
  std::vector<int> ids;
  // iterate all instance pins 
  for (odb::dbITerm* iterm : db_net->getITerms()) {
    PinType pin = PinType(iterm->getName(), iterm);
    odb::dbMTerm* mterm = iterm->getMTerm();
    odb::dbInst* inst = iterm->getInst();
    const odb::dbTransform transform = inst->getTransform();
    for (odb::dbMPin* mterm : mterm->getMPins()) { 
      for (odb::dbBox* box : mterm->getGeometry()) {
        odb::dbTechLayer* tech_layer = box->getTechLayer();
        if (tech_layer->getType() != odb::dbTechLayerType::ROUTING) {
          continue;
        }
        // get lower and upper layer
        odb::dbTechLayer* upper_layer = tech_layer->getUpperLayer();
        odb::dbTechLayer* lower_layer = tech_layer->getLowerLayer();

        odb::Rect pin_rect = box->getBox();
        transform.apply(pin_rect);
        // convert rect -> polygon
        Polygon pin_pol = rectToPolygon(pin_rect);
        // if has wire on same layer connect to pin
        ids = findNodesWithIntersection(node_by_layer_map_[tech_layer], pin_pol);
        for (const int& index : ids) {
          pin_nbrs[pin].push_back(node_by_layer_map_[tech_layer][index]->id);
        }
        // if has via on upper layer connected to pin
        if (upper_layer) {
          ids = findNodesWithIntersection(node_by_layer_map_[upper_layer], pin_pol);
          for (const int& index : ids) {
            pin_nbrs[pin].push_back(node_by_layer_map_[upper_layer][index]->id);
          }
        }
        // if has via on lower layer connected to pin
        if (lower_layer) {
          ids = findNodesWithIntersection(node_by_layer_map_[lower_layer], pin_pol);
          for (const int& index : ids) {
            pin_nbrs[pin].push_back(node_by_layer_map_[lower_layer][index]->id);
          }
        }
      }
    }
  }
  // run DSU from min_layer to max_layer
  init_dsu(); 
  dbTechLayer* iter = min_layer_;
  dbTechLayer* lower_layer; 
  while (iter) {
    // iterate each node of this layer to union set
    for( auto &node_it : node_by_layer_map_[iter] ) {
      int id_u = node_it->id;
      // if has lower layer
      lower_layer = iter->getLowerLayer();
      if (lower_layer) {
        // get lower neighbors and union
        for (const int& lower_it: node_it->low_adj) {
          int id_v = node_by_layer_map_[lower_layer][lower_it]->id;
          // if they are on different sets then union 
          if (!dsu_same(id_u, id_v)) {
            union_set(id_u, id_v);
          }
        }
      }
    }
    for( auto &node_it : node_by_layer_map_[iter] ) {
      int id_u = node_it->id;
      // check gates in same set (first Nodes x gates)
      for (const auto& gate_it : pin_nbrs) {
        for (const int& nbr_id : gate_it.second) {
          if (dsu_same(id_u, nbr_id)) {
            node_it->gates.insert(gate_it.first);
            break;
          }
        }
      }
    }
    iter = iter->getUpperLayer();
  }
}

bool AntennaChecker::isValidGate (odb::dbMTerm* mterm) {
  return mterm->getIoType() == dbIoType::INPUT && gateArea(mterm) > 0.0;
}

void AntennaChecker::calculateWirePar(dbTechLayer* tech_layer, InfoType& info) {
  // get info from layer map
  const double diff_metal_factor = layer_info_[tech_layer].diff_metal_factor;
  const double diff_side_metal_factor = layer_info_[tech_layer].diff_side_metal_factor;
  const double minus_diff_factor = layer_info_[tech_layer].minus_diff_factor;
  const double plus_diff_factor = layer_info_[tech_layer].plus_diff_factor;

  const double metal_factor = layer_info_[tech_layer].metal_factor;
  const double side_metal_factor = layer_info_[tech_layer].side_metal_factor;

  double diff_metal_reduce_factor = 1.0;
  if (tech_layer->hasDefaultAntennaRule()) {
    const dbTechLayerAntennaRule* antenna_rule = tech_layer->getDefaultAntennaRule();
    diff_metal_reduce_factor = getPwlFactor(antenna_rule->getAreaDiffReduce(), info.iterm_diff_area, 1.0);
  }

  if (info.iterm_diff_area != 0) {
    // Calculate PAR
    info.PAR = (diff_metal_factor * info.area) / info.iterm_gate_area;
    info.PSR = (diff_side_metal_factor * info.side_area) / info.iterm_gate_area;
  
    // Calculate PSR
    info.diff_PAR = (diff_metal_factor * info.area * diff_metal_reduce_factor - minus_diff_factor * info.iterm_diff_area) / (info.iterm_gate_area + plus_diff_factor * info.iterm_diff_area);
    info.diff_PSR = (diff_side_metal_factor * info.side_area * diff_metal_reduce_factor - minus_diff_factor * info.iterm_diff_area) / (info.iterm_gate_area + plus_diff_factor * info.iterm_diff_area);
  } else {
    // Calculate PAR
    info.PAR = (metal_factor * info.area) / info.iterm_gate_area;
    info.PSR = (side_metal_factor * info.side_area) / info.iterm_gate_area;

    // Calculate PSR
    info.diff_PAR = (metal_factor * info.area * diff_metal_reduce_factor) / info.iterm_gate_area;
    info.diff_PSR = (side_metal_factor * info.side_area * diff_metal_reduce_factor) / info.iterm_gate_area;
  } 
}

void AntennaChecker::calculateViaPar(dbTechLayer* tech_layer, InfoType& info) {
  // get info from layer map
  const double diff_cut_factor = layer_info_[tech_layer].diff_cut_factor;
  const double minus_diff_factor = layer_info_[tech_layer].minus_diff_factor;
  const double plus_diff_factor = layer_info_[tech_layer].plus_diff_factor;
  const double cut_factor = layer_info_[tech_layer].cut_factor;

  double diff_metal_reduce_factor = 1.0;
  if (tech_layer->hasDefaultAntennaRule()) {
    const dbTechLayerAntennaRule* antenna_rule = tech_layer->getDefaultAntennaRule();
    diff_metal_reduce_factor = getPwlFactor(antenna_rule->getAreaDiffReduce(), info.iterm_diff_area, 1.0);
  }

  if (info.iterm_diff_area != 0) {
    // Calculate PAR
    info.PAR = (diff_cut_factor * info.area) / info.iterm_gate_area;
    // Calculate diff_PAR
    info.diff_PAR = (diff_cut_factor * info.area * diff_metal_reduce_factor - minus_diff_factor * info.iterm_diff_area) / (info.iterm_gate_area + plus_diff_factor * info.iterm_diff_area);
  } else {
    // Calculate PAR
    info.PAR = (cut_factor * info.area) / info.iterm_gate_area;
    // Calculate diff_PAR
    info.diff_PAR = (cut_factor * info.area * diff_metal_reduce_factor) / info.iterm_gate_area;
  }
}

void AntennaChecker::calculateAreas () {
  for (const auto& it: node_by_layer_map_) {
    for (const auto& node_it: it.second) { 
      InfoType info;
      info.area = dbuToMicrons(dbuToMicrons(gtl::area(node_it->pol)));
      int gates_count = 0;
      vector<dbITerm*> iterms;
      for (const auto& gate: node_it->gates) {
        if (!gate.isITerm) continue;
        info.iterms.push_back(gate.iterm);
        info.iterm_gate_area += gateArea(gate.iterm->getMTerm());
        info.iterm_diff_area += diffArea(gate.iterm->getMTerm());
        gates_count++;
      }
      if (gates_count == 0) continue;

      if (it.first->getRoutingLevel() != 0) {
        // Calculate side area of wire
        uint wire_thickness_dbu = 0;
        it.first->getThickness(wire_thickness_dbu);
        double wire_thickness = dbuToMicrons(wire_thickness_dbu);
        info.side_area = dbuToMicrons(gtl::perimeter(node_it->pol) * wire_thickness);
      }
      // put values on struct
       for (const auto& gate: node_it->gates) {
        if (!gate.isITerm) continue;
        if (!isValidGate(gate.iterm->getMTerm()))continue;
        // check if has another node with gate in the layer, then merge area
        if (info_[gate.name].find(it.first) != info_[gate.name].end()) {
          info_[gate.name][it.first] += info;
        } else {
          info_[gate.name][it.first] = info;
        }
      }
    }
  }
}

// calculate PAR and PSR of wires and vias
void AntennaChecker::calculatePAR() {
  for (auto& gate_it: info_) {
    for (auto& layer_it: gate_it.second) {
      InfoType& gate_info = layer_it.second;
      dbTechLayer* tech_layer = layer_it.first;
      InfoType info;
      if (tech_layer->getRoutingLevel() == 0) {
        calculateViaPar(tech_layer, gate_info);
      } else {
        calculateWirePar(tech_layer, gate_info);
      }
    }
  }
}

// calculate CAR and CSR of wires and vias
void AntennaChecker::calculateCAR(){
  for (auto& gate_it: info_) {
    InfoType sumWire, sumVia;
    // iterate from first_layer -> last layer, cumulate sum for wires and vias
    dbTechLayer* iter_layer = min_layer_;
    while (iter_layer) {
      if (gate_it.second.find(iter_layer) != gate_it.second.end()) {
        if (iter_layer->getRoutingLevel() == 0) {
          sumVia += gate_it.second[iter_layer];
          gate_it.second[iter_layer].CAR += sumVia.PAR;
          gate_it.second[iter_layer].CSR += sumVia.PSR;
          gate_it.second[iter_layer].diff_CAR += sumVia.diff_PAR;
          gate_it.second[iter_layer].diff_CSR += sumVia.diff_PSR;
        } else {
          sumWire += gate_it.second[iter_layer]; 
          gate_it.second[iter_layer].CAR += sumWire.PAR;
          gate_it.second[iter_layer].CSR += sumWire.PSR;
          gate_it.second[iter_layer].diff_CAR += sumWire.diff_PAR;
          gate_it.second[iter_layer].diff_CSR += sumWire.diff_PSR;
        }
      }
      iter_layer = iter_layer->getUpperLayer();
    }
    
  }
}

std::pair<bool, bool> AntennaChecker::checkPAR(dbTechLayer * tech_layer, const InfoType& info, bool verbose, bool report, std::ofstream& report_file) {
  // get rules
  const dbTechLayerAntennaRule* antenna_rule = tech_layer->getDefaultAntennaRule();
  double PAR_ratio = antenna_rule->getPAR();
  dbTechLayerAntennaRule::pwl_pair diffPAR = antenna_rule->getDiffPAR();
  double diff_PAR_PWL_ratio = getPwlFactor(diffPAR, info.iterm_diff_area, 0.0);
  bool checked = false;

  // apply ratio_margin
  PAR_ratio *= (1.0 - ratio_margin_ / 100.0);
  diff_PAR_PWL_ratio *= (1.0 - ratio_margin_ / 100.0);

  // check PAR or diff_PAR
  if (PAR_ratio != 0) {
    bool par_violation = info.PAR > PAR_ratio;
    if ( (par_violation && report) || verbose) {
      std::string par_report = fmt::format(
          "      Partial area ratio: {:7.2f}\n      Required ratio: "
          "{:7.2f} "
          "(Gate area) {}",
          info.PAR,
          PAR_ratio,
          par_violation ? "(VIOLATED)" : "");
      logger_->report("{}", par_report);
      if (report_file.is_open()) {
        report_file << par_report << "\n";
      }
    }
    return {par_violation, checked};
  }
  else {
    bool diff_par_violation = false;
    if (diff_PAR_PWL_ratio != 0) {
      checked = true;
      diff_par_violation =  info.diff_PAR > diff_PAR_PWL_ratio;
    }
    if ((diff_par_violation && report) || verbose) {
      std::string diff_par_report = fmt::format(
          "      Partial area ratio: {:7.2f}\n      Required ratio: "
          "{:7.2f} "
          "(Gate area) {}",
          info.diff_PAR,
          diff_PAR_PWL_ratio,
          diff_par_violation ? "(VIOLATED)" : "");
      logger_->report("{}", diff_par_report);
      if (report_file.is_open()) {
        report_file << diff_par_report << "\n";
      }
    }
    return {diff_par_violation, checked};
  }
}

std::pair<bool, bool> AntennaChecker::checkPSR(dbTechLayer* tech_layer, const InfoType& info, bool verbose, bool report, std::ofstream& report_file) {
  // get rules
  const dbTechLayerAntennaRule* antenna_rule = tech_layer->getDefaultAntennaRule();
  double PSR_ratio = antenna_rule->getPSR();
  const dbTechLayerAntennaRule::pwl_pair diffPSR = antenna_rule->getDiffPSR();
  double diff_PSR_PWL_ratio = getPwlFactor(diffPSR, info.iterm_diff_area, 0.0);
  bool checked = false;

  // apply ratio_margin
  PSR_ratio *= (1.0 - ratio_margin_ / 100.0);
  diff_PSR_PWL_ratio *= (1.0 - ratio_margin_ / 100.0);

  // check PSR or diff_PSR
  if (PSR_ratio != 0) {
    bool psr_violation = info.PSR > PSR_ratio;
    if ((psr_violation && report) || verbose) {
      std::string psr_report = fmt::format(
          "      Partial area ratio: {:7.2f}\n      Required ratio: "
          "{:7.2f} "
          "(Side area) {}",
          info.PSR,
          PSR_ratio,
          psr_violation ? "(VIOLATED)" : "");
      logger_->report("{}", psr_report);
      if (report_file.is_open()) {
        report_file << psr_report << "\n";
      }
    }
    return {psr_violation, checked};
  }
  else {
    bool diff_psr_violation = false;
    if (diff_PSR_PWL_ratio != 0) {
      checked = true;
      diff_psr_violation = info.diff_PSR > diff_PSR_PWL_ratio;
    }
    if ((diff_psr_violation && report) || verbose) {
      std::string diff_psr_report = fmt::format(
          "      Partial area ratio: {:7.2f}\n      Required ratio: "
          "{:7.2f} "
          "(Side area) {}",
          info.diff_PSR,
          diff_PSR_PWL_ratio,
          diff_psr_violation ? "(VIOLATED)" : "");
      logger_->report("{}", diff_psr_report);
      if (report_file.is_open()) {
        report_file << diff_psr_report << "\n";
      }
    }
    return {diff_psr_violation, checked};
  }
}

bool AntennaChecker::checkCAR(dbTechLayer* tech_layer, const InfoType& info, bool verbose, bool report, std::ofstream& report_file) {
  // get rules
  const dbTechLayerAntennaRule* antenna_rule = tech_layer->getDefaultAntennaRule();
  const double CAR_ratio = antenna_rule->getCAR();
  const dbTechLayerAntennaRule::pwl_pair diffCAR = antenna_rule->getDiffCAR();
  const double diff_CAR_PWL_ratio = getPwlFactor(diffCAR, info.iterm_diff_area, 0);

  // check CAR or diff_CAR
  if (CAR_ratio != 0) {
    bool car_violation = info.CAR > CAR_ratio;
    if ((car_violation && report) || verbose) {
      std::string car_report = fmt::format(
          "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
          "{:7.2f} "
          "(Cumulative area) {}",
          info.CAR,
          CAR_ratio,
          car_violation ? "(VIOLATED)" : "");
      logger_->report("{}", car_report);
      if (report_file.is_open()) {
        report_file << car_report << "\n";
      }
    }
    return car_violation;
  }
  else {
    bool diff_car_violation = false;
    if (diff_CAR_PWL_ratio != 0) {
      diff_car_violation = info.diff_CAR > diff_CAR_PWL_ratio;
    }
    if ((diff_car_violation && report) || verbose) {
      std::string diff_car_report = fmt::format(
          "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
          "{:7.2f} "
          "(Cumulative area) {}",
          info.diff_CAR,
          diff_CAR_PWL_ratio,
          diff_car_violation ? "(VIOLATED)" : "");
      logger_->report("{}", diff_car_report);
      if (report_file.is_open()) {
        report_file << diff_car_report << "\n";
      }
    }
    return diff_car_violation;
  }
}

bool AntennaChecker::checkCSR(dbTechLayer* tech_layer, const InfoType& info, bool verbose, bool report, std::ofstream& report_file) {
  // get rules
  const dbTechLayerAntennaRule* antenna_rule = tech_layer->getDefaultAntennaRule();
  const double CSR_ratio = antenna_rule->getCSR();
  const dbTechLayerAntennaRule::pwl_pair diffCSR = antenna_rule->getDiffCSR();
  const double diff_CSR_PWL_ratio = getPwlFactor(diffCSR, info.iterm_diff_area, 0);

  // check CSR or diff_CSR
  if (CSR_ratio != 0) {
    bool csr_violation = info.CSR > CSR_ratio;
    if ((csr_violation && report) || verbose) {
      std::string csr_report = fmt::format(
          "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
          "{:7.2f} "
          "(Cumulative side area) {}",
          info.CSR,
          CSR_ratio,
          csr_violation ? "(VIOLATED)" : "");
      logger_->report("{}", csr_report);
      if (report_file.is_open()) {
        report_file << csr_report << "\n";
      }
    }
    return csr_violation;
  }
  else {
    bool diff_csr_violation = false;
    if (diff_CSR_PWL_ratio != 0) {
      diff_csr_violation = info.diff_CSR > diff_CSR_PWL_ratio;
    }
    if ((diff_csr_violation && report) || verbose) {
      std::string diff_csr_report = fmt::format(
          "      Cumulative area ratio: {:7.2f}\n      Required ratio: "
          "{:7.2f} "
          "(Cumulative side area) {}",
          info.diff_CSR,
          diff_CSR_PWL_ratio,
          diff_csr_violation ? "(VIOLATED)" : "");
      logger_->report("{}", diff_csr_report);
      if (report_file.is_open()) {
        report_file << diff_csr_report << "\n";
      }
    }
    return diff_csr_violation;
  }
}

int AntennaChecker::checkInfo(dbNet* db_net, bool verbose, bool report, std::ofstream& report_file, dbMTerm* diode_mterm, float ratio_margin) { 

  ratio_margin_ = ratio_margin;
  int pin_violation_count = 0;
  bool net_is_reported, pin_is_reported, layer_is_reported;

  std::unordered_map<dbTechLayer*, std::unordered_set<std::string>> pin_added;

  net_is_reported = false;
  for (const auto& gate_info : info_) {
    bool pin_has_violation = false;
    pin_is_reported = false;

    for (const auto& layer_info : gate_info.second) {
      layer_is_reported = false;
      bool node_has_violation = false;
      if (layer_info.first->hasDefaultAntennaRule()) {
        // check if node has violation
        if (layer_info.first->getRoutingLevel() != 0) {
          auto par_violation = checkPAR(layer_info.first, layer_info.second, false, false, report_file);
          auto psr_violation = checkPSR(layer_info.first, layer_info.second, false, false, report_file);
          bool car_violation = checkCAR(layer_info.first, layer_info.second, false, false, report_file);
          bool csr_violation = checkCSR(layer_info.first, layer_info.second, false, false, report_file);

          if (par_violation.first || psr_violation.first || car_violation || csr_violation) {
            node_has_violation = true;
          }
        }
        else {
          auto par_violation = checkPAR(layer_info.first, layer_info.second, false, false, report_file);
          bool car_violation = checkCAR(layer_info.first, layer_info.second, false, false, report_file);
          if (par_violation.first || car_violation) {
            node_has_violation = true;
          }
        }

        // If verbose or report is on
        if ((node_has_violation || report) && diode_mterm == nullptr) {
          if (!net_is_reported) {
            std::string net_name = fmt::format("Net: {}", db_net->getConstName());
            logger_->report("{}", net_name);
            if (report_file.is_open()) {
              report_file << net_name << "\n";
            }
            net_is_reported = true;
          }
          if(!pin_is_reported) {
            std::string pin_name = fmt::format("  Pin: {}", gate_info.first);
            logger_->report("{}", pin_name);
            if (report_file.is_open()) {
              report_file << pin_name << "\n";
            }
            pin_is_reported = true;
          }
          if(!layer_is_reported) {
            std::string layer_name = fmt::format("    Layer: {}", layer_info.first->getConstName());
            logger_->report("{}", layer_name);
            if (report_file.is_open()) {
              report_file << layer_name << "\n";
            }
            layer_is_reported = true;
          }
 
          // re-check to report violations
          if (layer_info.first->getRoutingLevel() != 0) {
            auto par_violation = checkPAR(layer_info.first, layer_info.second, verbose, true, report_file);
            auto psr_violation = checkPSR(layer_info.first, layer_info.second, verbose, true, report_file);
            bool car_violation = checkCAR(layer_info.first, layer_info.second, verbose, true, report_file);
            bool csr_violation = checkCSR(layer_info.first, layer_info.second, verbose, true, report_file);
          }
          else {
            auto par_violation = checkPAR(layer_info.first, layer_info.second, verbose, true, report_file);
            bool car_violation = checkCAR(layer_info.first, layer_info.second, verbose, true, report_file);
          }
        }

        if (node_has_violation) {
          pin_has_violation = true;
          // when repair antenna is running, calculate number of diodes
          if (diode_mterm && layer_info.first->getRoutingLevel() != 0 && pin_added[layer_info.first].find(gate_info.first) == pin_added[layer_info.first].end()) {
            double diode_diff_area = 0.0;
            if (diode_mterm) {
              diode_diff_area = diffArea(diode_mterm);
            }
            InfoType violation_info = layer_info.second;
            std::vector<dbITerm*> gates = violation_info.iterms;
            dbTechLayer* violation_layer = layer_info.first;
            int diode_count_per_gate = 0;
            // check violations only PAR & PSR
            auto par_violation = checkPAR(violation_layer, violation_info, false, false, report_file);
            auto psr_violation = checkPSR(violation_layer, violation_info, false, false, report_file);
            // while it has violation, increase iterm_diff_area
            while (par_violation.first || psr_violation.first) {
              // increasing iterm_diff_area and count
              violation_info.iterm_diff_area += diode_diff_area * gates.size();
              diode_count_per_gate++; 
              // re-calculate info only PAR & PSR
              calculateWirePar(violation_layer, violation_info);
              // re-check violations only PAR & PSR
              par_violation = checkPAR(violation_layer, violation_info, false, false, report_file);
              psr_violation = checkPSR(violation_layer, violation_info, false, false, report_file);
              if (diode_count_per_gate > max_diode_count_per_gate) {
                logger_->warn(ANT,
                              15,
                              "Net {} requires more than {} diodes per gate to "
                              "repair violations.",
                              db_net->getConstName(),
                              max_diode_count_per_gate);
                break;
              }
            }
            // save the iterms of repaired node
            for (const auto& iterm_iter : gates) {
              pin_added[violation_layer].insert(iterm_iter->getName());
            }
            // save antenna violation
            if (diode_count_per_gate > 0) {
              antenna_violations_.push_back({layer_info.first->getRoutingLevel(), gates, diode_count_per_gate});
            }
          }
        }
      }
    }
    if (pin_has_violation) {
      pin_violation_count++;
    }

    if (pin_is_reported) {
      logger_->report("");
      if (report_file.is_open()) {
        report_file << "\n";
      }
    }
  }

  if (net_is_reported) {
    logger_->report("");
    if (report_file.is_open()) {
      report_file << "\n";
    }
  }
  return pin_violation_count;
}

void wiresToPolygonSetMap(dbWire* wires, std::unordered_map<odb::dbTechLayer*, PolygonSet>& set_by_layer) {

  odb::dbShape shape;
  odb::dbWireShapeItr shapes_it;
  std::vector<odb::dbShape> via_boxes;

  // Add information on polygon sets
  for (shapes_it.begin(wires); shapes_it.next(shape);) {

    odb::dbTechLayer* layer;

    // Get rect of the wire
    odb::Rect wire_rect = shape.getBox(); 

    if (shape.isVia()) {
      // Get three polygon upper_cut - via - lower_cut
      odb::dbShape::getViaBoxes(shape, via_boxes);
      for (const odb::dbShape& box : via_boxes) {
        layer = box.getTechLayer();
        odb::Rect via_rect = box.getBox();
        Polygon via_pol = rectToPolygon(via_rect);
        set_by_layer[layer] += via_pol;
      }
    }
    else {
      layer = shape.getTechLayer();
      // polygon set is used to join polygon on same layer with intersection
      Polygon wire_pol = rectToPolygon(wire_rect);
      set_by_layer[layer] += wire_pol; 
    }
  }
}

void avoidPinIntersection(dbNet* db_net, std::unordered_map<odb::dbTechLayer*, PolygonSet>& set_by_layer){
  // iterate all instance pin
  for (odb::dbITerm* iterm : db_net->getITerms()) {
    odb::dbMTerm* mterm = iterm->getMTerm();
    odb::dbInst* inst = iterm->getInst();
    const odb::dbTransform transform = inst->getTransform();
    for (odb::dbMPin* mterm : mterm->getMPins()) {
      for (odb::dbBox* box : mterm->getGeometry()) {
        odb::dbTechLayer* tech_layer = box->getTechLayer();
        if (tech_layer->getType() != odb::dbTechLayerType::ROUTING) {
          continue;
        }

        odb::Rect pin_rect = box->getBox();
        transform.apply(pin_rect);
        // convert rect -> polygon
        Polygon pin_pol = rectToPolygon(pin_rect);
        // Remove the area with intersection of the polygon set
        set_by_layer[tech_layer] -= pin_pol;
      }
    }
  }
}

void AntennaChecker::buildLayerMaps(dbNet* db_net) {
  dbWire* wires = db_net->getWire();

  std::unordered_map<odb::dbTechLayer*, PolygonSet> set_by_layer;

  wiresToPolygonSetMap(wires, set_by_layer);
  avoidPinIntersection(db_net, set_by_layer);
 
  // init struct (copy polygon set information on struct to save neighbors)
  node_by_layer_map_.clear();
  info_.clear();
  node_count_ = 0;
  odb::dbTech* tech = db_->getTech();
  min_layer_ = tech->findRoutingLayer(1);

  for (const auto & layer_it : set_by_layer){
    for (const auto & pol_it : layer_it.second){
      bool isVia = layer_it.first->getRoutingLevel() == 0;
      node_by_layer_map_[layer_it.first].push_back(new GraphNode(node_count_, isVia, pol_it));
      node_count_++;
    }
  }

  // set connections between Polygons ( wire -> via -> wire)
  std::vector<int> upper_index, lower_index;
  for (const auto &layer_it : set_by_layer) {
    // iterate only via layers
    if (layer_it.first->getRoutingLevel() == 0) {
      int via_index = 0;
      for (const auto &via_it : layer_it.second) {

        lower_index = findNodesWithIntersection(node_by_layer_map_[layer_it.first->getLowerLayer()], via_it);
        upper_index = findNodesWithIntersection(node_by_layer_map_[layer_it.first->getUpperLayer()], via_it);

        if (upper_index.size() <= 2) {
          // connect upper -> via
          for (int& up_index : upper_index) {
            node_by_layer_map_[layer_it.first->getUpperLayer()][up_index]->low_adj.push_back(via_index);
          }
        } else if(upper_index.size() > 2) {
          std::string log_error = fmt::format("ERROR: net {} has via on {} conect with multiple wires on layer {} \n", db_net->getConstName(), layer_it.first->getName(), layer_it.first->getUpperLayer()->getName());
          logger_->report("{}", log_error);
        }
        if (lower_index.size() == 1) {
          //connect via -> lower
          for (int& low_index : lower_index) {
            node_by_layer_map_[layer_it.first][via_index]->low_adj.push_back(low_index);
          }
        } else if(lower_index.size() > 2) {
          std::string log_error = fmt::format("ERROR: net {} has via on {} conect with multiple wires on layer {} \n", db_net->getConstName(), layer_it.first->getName(), layer_it.first->getLowerLayer()->getName());
          logger_->report("{}", log_error);
        }
        via_index++;
      }
    }
  }
  saveGates(db_net);
}

void AntennaChecker::checkNet(dbNet* db_net, bool verbose, bool report, std::ofstream& report_file, dbMTerm* diode_mterm, float ratio_margin, int& net_violation_count, int& pin_violation_count) { 

  dbWire* wire = db_net->getWire();
  if (wire) {
    buildLayerMaps(db_net);

    calculateAreas();

    calculatePAR();
    calculateCAR();

    int pin_violations = checkInfo(db_net, verbose, report, report_file, diode_mterm, ratio_margin);

    if (pin_violations > 0) {
      net_violation_count++;
      pin_violation_count += pin_violations;
    }
  }
}

vector<Violation> AntennaChecker::getAntennaViolations2(dbNet* net,
                                                       dbMTerm* diode_mterm,
                                                       float ratio_margin)
{
  antenna_violations_.clear();
  if (net->isSpecial()) {
    return antenna_violations_;
  } 

  int net_violation_count, pin_violation_count;
  net_violation_count = 0;
  pin_violation_count = 0;
  std::ofstream report_file;
  checkNet(net, false, false, report_file, diode_mterm, ratio_margin, net_violation_count, pin_violation_count);

  return antenna_violations_;
}

//////////////////////////////////////////////////////////////////////////////////////////

void AntennaChecker::checkNet(dbNet* net,
                              bool report_if_no_violation,
                              bool verbose,
                              std::ofstream& report_file,
                              // Return values.
                              int& net_violation_count,
                              int& pin_violation_count,
                              bool use_grt_routes)
{
  dbWire* wire = net->getWire();
  if (wire) {
    std::vector<int> data;
    std::vector<unsigned char> op_code;
    if (!use_grt_routes) {
      wire->getRawWireData(data, op_code);
      odb::orderWires(logger_, net);
    }
    vector<dbWireGraph::Node*> wire_roots;
    vector<dbWireGraph::Node*> gate_nodes;
    findWireRoots(wire, wire_roots, gate_nodes);

    vector<PARinfo> PARtable = buildWireParTable(wire_roots);
    vector<PARinfo> VIA_PARtable = buildViaParTable(wire_roots);
    vector<ARinfo> CARtable
        = buildWireCarTable(PARtable, VIA_PARtable, gate_nodes);
    vector<ARinfo> VIA_CARtable
        = buildViaCarTable(PARtable, VIA_PARtable, gate_nodes);

    bool violation = false;
    unordered_set<dbWireGraph::Node*> violated_gates;
    for (dbWireGraph::Node* gate : gate_nodes) {
      checkGate(gate,
                CARtable,
                VIA_CARtable,
                false,
                verbose,
                report_file,
                violation,
                violated_gates);
    }

    if (violation) {
      net_violation_count++;
      pin_violation_count += violated_gates.size();
    }

    // Repeat with reporting.
    if (violation || report_if_no_violation) {
      std::string net_name = fmt::format("Net: {}", net->getConstName());

      if (report_file.is_open()) {
        report_file << net_name << "\n";
      }
      if (verbose) {
        logger_->report("{}", net_name);
      }

      for (dbWireGraph::Node* gate : gate_nodes) {
        checkGate(gate,
                  CARtable,
                  VIA_CARtable,
                  true,
                  verbose,
                  report_file,
                  violation,
                  violated_gates);
      }
      if (verbose) {
        logger_->report("");
      }
    }
    if (!use_grt_routes) {
      wire->setRawWireData(data, op_code);
    }
  }
}

void AntennaChecker::checkGate(
    dbWireGraph::Node* gate,
    vector<ARinfo>& CARtable,
    vector<ARinfo>& VIA_CARtable,
    bool report,
    bool verbose,
    std::ofstream& report_file,
    // Return values.
    bool& violation,
    unordered_set<dbWireGraph::Node*>& violated_gates)
{
  bool first_pin_violation = true;
  for (const auto& ar : CARtable) {
    if (ar.GateNode == gate) {
      auto wire_PAR_violation = checkWirePar(ar, false, verbose, report_file);

      auto wire_CAR_violation = checkWireCar(
          ar, wire_PAR_violation.second, false, verbose, report_file);
      bool wire_violation
          = wire_PAR_violation.first || wire_CAR_violation.first;
      violation |= wire_violation;
      if (wire_violation) {
        violated_gates.insert(gate);
      }

      if (report) {
        if (wire_violation || verbose) {
          if (first_pin_violation) {
            dbITerm* iterm = dbITerm::getITerm(block_, gate->object()->getId());
            dbMTerm* mterm = iterm->getMTerm();

            std::string mterm_info
                = fmt::format("  Pin: {}/{} ({})",
                              iterm->getInst()->getConstName(),
                              mterm->getConstName(),
                              mterm->getMaster()->getConstName());

            if (report_file.is_open()) {
              report_file << mterm_info << "\n";
            }
            if (verbose) {
              logger_->report("{}", mterm_info);
            }
          }

          std::string layer_name = fmt::format(
              "    Layer: {}", ar.par_info.wire_root->layer()->getConstName());

          if (report_file.is_open()) {
            report_file << layer_name << "\n";
          }
          if (verbose) {
            logger_->report("{}", layer_name);
          }
          first_pin_violation = false;
        }
        checkWirePar(ar, true, verbose, report_file);
        checkWireCar(ar, wire_PAR_violation.second, true, verbose, report_file);
        if (wire_violation || verbose) {
          if (report_file.is_open()) {
            report_file << "\n";
          }
          if (verbose) {
            logger_->report("");
          }
        }
      }
    }
  }
  for (const auto& via_ar : VIA_CARtable) {
    if (via_ar.GateNode == gate) {
      bool VIA_PAR_violation = checkViaPar(via_ar, false, verbose, report_file);
      bool VIA_CAR_violation = checkViaCar(via_ar, false, verbose, report_file);
      bool via_violation = VIA_PAR_violation || VIA_CAR_violation;
      violation |= via_violation;
      if (via_violation) {
        violated_gates.insert(gate);
      }

      if (report) {
        if (via_violation || verbose) {
          dbWireGraph::Edge* via
              = findVia(via_ar.par_info.wire_root,
                        via_ar.par_info.wire_root->layer()->getRoutingLevel());

          std::string via_name
              = fmt::format("    Via: {}", getViaName(via).c_str());
          if (report_file.is_open()) {
            report_file << via_name << "\n";
          }
          logger_->report("{}", via_name);
        }
        checkViaPar(via_ar, true, verbose, report_file);
        checkViaCar(via_ar, true, verbose, report_file);
        if (via_violation || verbose) {
          if (report_file.is_open()) {
            report_file << "\n";
          }
          if (verbose) {
            logger_->report("");
          }
        }
      }
    }
  }
}

int AntennaChecker::checkAntennas(dbNet* net, bool verbose)
{
  initAntennaRules();

  std::ofstream report_file;
  if (!report_file_name_.empty()) {
    report_file.open(report_file_name_, std::ofstream::out);
  }

  bool grt_routes = global_route_source_->haveRoutes();
  bool drt_routes = haveRoutedNets();
  bool use_grt_routes = (grt_routes && !drt_routes);
  if (!grt_routes && !drt_routes) {
    logger_->error(ANT,
                   8,
                   "No detailed or global routing found. Run global_route or "
                   "detailed_route first.");
  }

  if (use_grt_routes) {
    global_route_source_->makeNetWires();
  }

  int net_violation_count = 0;
  int pin_violation_count = 0;

  if (net) {
    if (!net->isSpecial()) {
      checkNet(net,
               verbose,
               true,
               report_file,
               nullptr,
               0,
               net_violation_count,
               pin_violation_count,
               use_grt_routes);
    } else {
      logger_->error(
          ANT, 14, "Skipped net {} because it is special.", net->getName());
    }
  } else {
    for (dbNet* net : block_->getNets()) {
      if (!net->isSpecial()) {
        checkNet(net,
                 verbose,
                 false,
                 report_file,
                 nullptr,
                 0,
                 net_violation_count,
                 pin_violation_count,
                 use_grt_routes);
      }
    }
  }

  logger_->info(ANT, 2, "Found {} net violations.", net_violation_count);
  logger_->metric("antenna__violating__nets", net_violation_count);
  logger_->info(ANT, 1, "Found {} pin violations.", pin_violation_count);
  logger_->metric("antenna__violating__pins", pin_violation_count);

  if (!report_file_name_.empty()) {
    report_file.close();
  }

  if (use_grt_routes) {
    global_route_source_->destroyNetWires();
  }

  net_violation_count_ = net_violation_count;
  return net_violation_count;
}

int AntennaChecker::antennaViolationCount() const
{
  return net_violation_count_;
}

bool AntennaChecker::haveRoutedNets()
{
  for (dbNet* net : block_->getNets()) {
    if (!net->isSpecial() && net->getWireType() == dbWireType::ROUTED
        && net->getWire()) {
      return true;
    }
  }
  return false;
}

void AntennaChecker::findWireRootIterms(dbWireGraph::Node* node,
                                        int wire_level,
                                        vector<dbITerm*>& gates)
{
  double iterm_gate_area = 0.0;
  double iterm_diff_area = 0.0;
  std::set<dbITerm*> iv;
  std::set<dbWireGraph::Node*> nv;

  findWireBelowIterms(
      node, iterm_gate_area, iterm_diff_area, wire_level, iv, nv);
  gates.assign(iv.begin(), iv.end());
}

vector<std::pair<double, vector<dbITerm*>>> AntennaChecker::parMaxWireLength(
    dbNet* net,
    int layer)
{
  vector<std::pair<double, vector<dbITerm*>>> par_wires;
  if (net->isSpecial()) {
    return par_wires;
  }
  dbWire* wire = net->getWire();
  if (wire != nullptr) {
    dbWireGraph graph;
    graph.decode(wire);

    std::set<dbWireGraph::Node*> level_nodes;
    vector<dbWireGraph::Node*> wire_roots = findWireRoots(wire);
    for (dbWireGraph::Node* wire_root : wire_roots) {
      odb::dbTechLayer* tech_layer = wire_root->layer();
      if (level_nodes.find(wire_root) == level_nodes.end()
          && tech_layer->getRoutingLevel() == layer) {
        double max_length = 0;
        std::set<dbWireGraph::Node*> nv;
        std::pair<double, double> areas = calculateWireArea(
            wire_root, tech_layer->getRoutingLevel(), nv, level_nodes);
        const double wire_area = areas.first;
        double iterm_gate_area = 0.0;
        double iterm_diff_area = 0.0;
        std::set<dbITerm*> iv;
        nv.clear();
        findWireBelowIterms(wire_root,
                            iterm_gate_area,
                            iterm_diff_area,
                            tech_layer->getRoutingLevel(),
                            iv,
                            nv);
        const double wire_width = block_->dbuToMicrons(tech_layer->getWidth());
        const AntennaModel& am = layer_info_[tech_layer];

        if (iterm_gate_area != 0 && tech_layer->hasDefaultAntennaRule()) {
          const dbTechLayerAntennaRule* antenna_rule
              = tech_layer->getDefaultAntennaRule();
          dbTechLayerAntennaRule::pwl_pair diff_metal_reduce_factor_pwl
              = antenna_rule->getAreaDiffReduce();
          double diff_metal_reduce_factor = getPwlFactor(
              diff_metal_reduce_factor_pwl, iterm_diff_area, 1.0);

          const double PAR_ratio = antenna_rule->getPAR();
          if (PAR_ratio != 0) {
            if (iterm_diff_area != 0) {
              max_length = (PAR_ratio * iterm_gate_area
                            - am.diff_metal_factor * wire_area)
                           / wire_width;
            } else {
              max_length
                  = (PAR_ratio * iterm_gate_area - am.metal_factor * wire_area)
                    / wire_width;
            }
          } else {
            dbTechLayerAntennaRule::pwl_pair diffPAR
                = antenna_rule->getDiffPAR();
            const double diffPAR_ratio
                = getPwlFactor(diffPAR, iterm_diff_area, 0.0);
            if (iterm_diff_area != 0) {
              max_length = (diffPAR_ratio
                                * (iterm_gate_area
                                   + am.plus_diff_factor * iterm_diff_area)
                            - (am.diff_metal_factor * wire_area
                                   * diff_metal_reduce_factor
                               - am.minus_diff_factor * iterm_diff_area))
                           / wire_width;
            } else {
              max_length
                  = (diffPAR_ratio
                         * (iterm_gate_area
                            + am.plus_diff_factor * iterm_diff_area)
                     - (am.metal_factor * wire_area * diff_metal_reduce_factor
                        - am.minus_diff_factor * iterm_diff_area))
                    / wire_width;
            }
          }
          if (max_length != 0) {
            vector<dbITerm*> gates;
            findWireRootIterms(
                wire_root, wire_root->layer()->getRoutingLevel(), gates);
            std::pair<double, vector<dbITerm*>> par_wire
                = std::make_pair(max_length, gates);
            par_wires.push_back(par_wire);
          }
        }
      }
    }
  }
  return par_wires;
}

bool AntennaChecker::checkViolation(const PARinfo& par_info, dbTechLayer* layer)
{
  const double par = par_info.PAR;
  const double psr = par_info.PSR;
  const double diff_par = par_info.diff_PAR;
  const double diff_psr = par_info.diff_PSR;
  const double diff_area = par_info.iterm_diff_area;

  if (layer->hasDefaultAntennaRule()) {
    const dbTechLayerAntennaRule* antenna_rule = layer->getDefaultAntennaRule();
    double PAR_ratio = antenna_rule->getPAR();
    PAR_ratio *= (1.0 - ratio_margin_ / 100.0);
    if (PAR_ratio != 0) {
      if (par > PAR_ratio) {
        return true;
      }
    } else {
      dbTechLayerAntennaRule::pwl_pair diffPAR = antenna_rule->getDiffPAR();
      double diffPAR_ratio = getPwlFactor(diffPAR, diff_area, 0.0);
      diffPAR_ratio *= (1.0 - ratio_margin_ / 100.0);

      if (diffPAR_ratio != 0 && diff_par > diffPAR_ratio) {
        return true;
      }
    }

    double PSR_ratio = antenna_rule->getPSR();
    PSR_ratio *= (1.0 - ratio_margin_ / 100.0);
    if (PSR_ratio != 0) {
      if (psr > PSR_ratio) {
        return true;
      }
    } else {
      dbTechLayerAntennaRule::pwl_pair diffPSR = antenna_rule->getDiffPSR();
      double diffPSR_ratio = getPwlFactor(diffPSR, diff_area, 0.0);
      diffPSR_ratio *= (1.0 - ratio_margin_ / 100.0);

      if (diffPSR_ratio != 0 && diff_psr > diffPSR_ratio) {
        return true;
      }
    }
  }

  return false;
}

vector<Violation> AntennaChecker::getAntennaViolations(dbNet* net,
                                                       dbMTerm* diode_mterm,
                                                       float ratio_margin)
{
  ratio_margin_ = ratio_margin;
  double diode_diff_area = 0.0;
  if (diode_mterm) {
    diode_diff_area = diffArea(diode_mterm);
  }

  vector<Violation> antenna_violations;
  if (net->isSpecial()) {
    return antenna_violations;
  }
  dbWire* wire = net->getWire();
  dbWireGraph graph;
  if (wire) {
    bool wire_was_oredered = net->isWireOrdered();
    std::vector<int> data;
    std::vector<unsigned char> op_code;
    if (!wire_was_oredered) {
      wire->getRawWireData(data, op_code);
      odb::orderWires(logger_, net);
    }
    auto wire_roots = findWireRoots(wire);

    vector<PARinfo> PARtable = buildWireParTable(wire_roots);
    for (PARinfo& par_info : PARtable) {
      dbTechLayer* layer = par_info.wire_root->layer();
      bool wire_PAR_violation = checkViolation(par_info, layer);

      if (wire_PAR_violation) {
        vector<dbITerm*> gates;
        findWireRootIterms(par_info.wire_root, layer->getRoutingLevel(), gates);
        int diode_count_per_gate = 0;
        if (diode_mterm && antennaRatioDiffDependent(layer)) {
          while (wire_PAR_violation) {
            par_info.iterm_diff_area += diode_diff_area * gates.size();
            diode_count_per_gate++;
            calculateParInfo(par_info);
            wire_PAR_violation = checkViolation(par_info, layer);
            if (diode_count_per_gate > max_diode_count_per_gate) {
              logger_->warn(ANT,
                            9,
                            "Net {} requires more than {} diodes per gate to "
                            "repair violations.",
                            net->getConstName(),
                            max_diode_count_per_gate);
              break;
            }
          }
        }
        Violation antenna_violation = {
            layer->getRoutingLevel(), std::move(gates), diode_count_per_gate};
        antenna_violations.push_back(antenna_violation);
      }
    }
    if (!wire_was_oredered) {
      wire->setRawWireData(data, op_code);
    }
  }
  return antenna_violations;
}

bool AntennaChecker::antennaRatioDiffDependent(dbTechLayer* layer)
{
  if (layer->hasDefaultAntennaRule()) {
    const dbTechLayerAntennaRule* antenna_rule = layer->getDefaultAntennaRule();
    dbTechLayerAntennaRule::pwl_pair diffPAR = antenna_rule->getDiffPAR();
    dbTechLayerAntennaRule::pwl_pair diffPSR = antenna_rule->getDiffPSR();
    return !diffPAR.indices.empty() || !diffPSR.indices.empty();
  }
  return false;
}

double AntennaChecker::diffArea(dbMTerm* mterm)
{
  double max_diff_area = 0.0;
  vector<std::pair<double, dbTechLayer*>> diff_areas;
  mterm->getDiffArea(diff_areas);
  for (const auto& [area, layer] : diff_areas) {
    max_diff_area = std::max(max_diff_area, area);
  }
  return max_diff_area;
}

vector<std::pair<double, vector<dbITerm*>>>
AntennaChecker::getViolatedWireLength(dbNet* net, int routing_level)
{
  vector<std::pair<double, vector<dbITerm*>>> violated_wires;
  if (net->isSpecial() || net->getWire() == nullptr) {
    return violated_wires;
  }
  dbWire* wire = net->getWire();

  dbWireGraph graph;
  std::set<dbWireGraph::Node*> level_nodes;
  for (dbWireGraph::Node* wire_root : findWireRoots(wire)) {
    odb::dbTechLayer* tech_layer = wire_root->layer();
    if (level_nodes.find(wire_root) == level_nodes.end()
        && tech_layer->getRoutingLevel() == routing_level) {
      std::set<dbWireGraph::Node*> nv;
      auto areas = calculateWireArea(
          wire_root, tech_layer->getRoutingLevel(), nv, level_nodes);
      double wire_area = areas.first;
      double iterm_gate_area = 0.0;
      double iterm_diff_area = 0.0;

      std::set<dbITerm*> iv;
      nv.clear();
      findWireBelowIterms(wire_root,
                          iterm_gate_area,
                          iterm_diff_area,
                          tech_layer->getRoutingLevel(),
                          iv,
                          nv);
      if (iterm_gate_area == 0) {
        continue;
      }

      double wire_width = block_->dbuToMicrons(tech_layer->getWidth());

      AntennaModel& am = layer_info_[tech_layer];
      double metal_factor = am.metal_factor;
      double diff_metal_factor = am.diff_metal_factor;

      double minus_diff_factor = am.minus_diff_factor;
      double plus_diff_factor = am.plus_diff_factor;

      if (wire_root->layer()->hasDefaultAntennaRule()) {
        const dbTechLayerAntennaRule* antenna_rule
            = tech_layer->getDefaultAntennaRule();
        double diff_metal_reduce_factor = getPwlFactor(
            antenna_rule->getAreaDiffReduce(), iterm_diff_area, 1.0);

        double par = 0;
        double diff_par = 0;

        if (iterm_diff_area != 0) {
          par = (diff_metal_factor * wire_area) / iterm_gate_area;
          diff_par = (diff_metal_factor * wire_area * diff_metal_reduce_factor
                      - minus_diff_factor * iterm_diff_area)
                     / (iterm_gate_area + plus_diff_factor * iterm_diff_area);
        } else {
          par = (metal_factor * wire_area) / iterm_gate_area;
          diff_par = (metal_factor * wire_area * diff_metal_reduce_factor)
                     / iterm_gate_area;
        }

        double cut_length = 0;
        const double PAR_ratio = antenna_rule->getPAR();
        if (PAR_ratio != 0) {
          if (par > PAR_ratio) {
            if (iterm_diff_area != 0) {
              cut_length = ((par - PAR_ratio) * iterm_gate_area
                            - diff_metal_factor * wire_area)
                           / wire_width;
            } else {
              cut_length = ((par - PAR_ratio) * iterm_gate_area
                            - metal_factor * wire_area)
                           / wire_width;
            }
          }

        } else {
          dbTechLayerAntennaRule::pwl_pair diffPAR = antenna_rule->getDiffPAR();
          const double diffPAR_ratio
              = getPwlFactor(diffPAR, iterm_diff_area, 0.0);
          if (iterm_diff_area != 0) {
            cut_length
                = ((diff_par - diffPAR_ratio)
                       * (iterm_gate_area + plus_diff_factor * iterm_diff_area)
                   - (diff_metal_factor * wire_area * diff_metal_reduce_factor
                      - minus_diff_factor * iterm_diff_area))
                  / wire_width;
          } else {
            cut_length
                = ((diff_par - diffPAR_ratio)
                       * (iterm_gate_area + plus_diff_factor * iterm_diff_area)
                   - (metal_factor * wire_area * diff_metal_reduce_factor
                      - minus_diff_factor * iterm_diff_area))
                  / wire_width;
          }
        }

        if (cut_length != 0) {
          vector<dbITerm*> gates;
          findWireRootIterms(wire_root, routing_level, gates);
          std::pair<double, vector<dbITerm*>> violated_wire
              = std::make_pair(cut_length, gates);
          violated_wires.push_back(violated_wire);
        }
      }
    }
  }
  return violated_wires;
}

void AntennaChecker::findMaxWireLength()
{
  dbNet* max_wire_net = nullptr;
  double max_wire_length = 0.0;

  for (dbNet* net : block_->getNets()) {
    dbWire* wire = net->getWire();
    if (wire && !net->isSpecial()) {
      dbWireGraph graph;
      graph.decode(wire);

      double wire_length = 0;
      dbWireGraph::edge_iterator edge_itr;
      for (edge_itr = graph.begin_edges(); edge_itr != graph.end_edges();
           ++edge_itr) {
        dbWireGraph::Edge* edge = *edge_itr;
        int x1, y1, x2, y2;
        edge->source()->xy(x1, y1);
        edge->target()->xy(x2, y2);
        if (edge->type() == dbWireGraph::Edge::Type::SEGMENT
            || edge->type() == dbWireGraph::Edge::Type::SHORT) {
          wire_length += block_->dbuToMicrons((abs(x2 - x1) + abs(y2 - y1)));
        }
      }

      if (wire_length > max_wire_length) {
        max_wire_length = wire_length;
        max_wire_net = net;
      }
    }
  }
  if (max_wire_net) {
    logger_->report(
        "net {} length {}", max_wire_net->getConstName(), max_wire_length);
  }
}

void AntennaChecker::setReportFileName(const char* file_name)
{
  report_file_name_ = file_name;
}

}  // namespace ant
