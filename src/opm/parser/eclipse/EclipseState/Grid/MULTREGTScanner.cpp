/*
  Copyright 2014 Statoil ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdexcept>
#include <map>
#include <set>

#include <opm/parser/eclipse/Deck/DeckItem.hpp>
#include <opm/parser/eclipse/Deck/DeckKeyword.hpp>
#include <opm/parser/eclipse/Deck/DeckRecord.hpp>
#include <opm/parser/eclipse/EclipseState/Eclipse3DProperties.hpp>
#include <opm/parser/eclipse/EclipseState/Grid/FaceDir.hpp>
#include <opm/parser/eclipse/EclipseState/Grid/GridProperties.hpp>
#include <opm/parser/eclipse/EclipseState/Grid/MULTREGTScanner.hpp>

namespace Opm {

    namespace MULTREGT {

        std::string RegionNameFromDeckValue(const std::string& stringValue) {
            if (stringValue == "O")
                return "OPERNUM";

            if (stringValue == "F")
                return "FLUXNUM";

            if (stringValue == "M")
                return "MULTNUM";

            throw std::invalid_argument("The input string: " + stringValue + " was invalid. Expected: O/F/M");
        }



        NNCBehaviourEnum NNCBehaviourFromString(const std::string& stringValue) {
            if (stringValue == "ALL")
                return ALL;

            if (stringValue == "NNC")
                return NNC;

            if (stringValue == "NONNC")
                return NONNC;

            if (stringValue == "NOAQUNNC")
                return NOAQUNNC;

            throw std::invalid_argument("The input string: " + stringValue + " was invalid. Expected: ALL/NNC/NONNC/NOAQUNNC");
        }


    }



    /*****************************************************************/
    /*
      Observe that the (REGION1 -> REGION2) pairs behave like keys;
      i.e. for the MULTREGT keyword

        MULTREGT
          2  4   0.75    Z   ALL    M /
          2  4   2.50   XY   ALL    F /
        /

      The first record is completely overweritten by the second
      record, this is because the both have the (2 -> 4) region
      identifiers. This behaviourt is ensured by using a map with
      std::pair<region1,region2> as key.

      This function starts with some initial preprocessing to create a
      map which looks like this:


         searchMap = {"MULTNUM" : {std::pair(1,2) : std::tuple(TransFactor , Face , Region),
                                   std::pair(4,7) : std::tuple(TransFactor , Face , Region),
                                   ...},
                      "FLUXNUM" : {std::pair(4,8) : std::tuple(TransFactor , Face , Region),
                                   std::pair(1,4) : std::tuple(TransFactor , Face , Region),
                                   ...}}

      Then it will go through the different regions and looking for
      interface with the wanted region values.
    */
    MULTREGTScanner::MULTREGTScanner(const Eclipse3DProperties& e3DProps,
                                     const std::vector< const DeckKeyword* >& keywords) :
                m_e3DProps(e3DProps) {

        for (size_t idx = 0; idx < keywords.size(); idx++)
            this->addKeyword(e3DProps, *keywords[idx] , e3DProps.getDefaultRegionKeyword());

        MULTREGTSearchMap searchPairs;
        for (std::vector<MULTREGTRecord>::const_iterator record = m_records.begin(); record != m_records.end(); ++record) {
            if (e3DProps.hasDeckIntGridProperty( record->region_name)) {
                int srcRegion    = record->src_value;
                int targetRegion = record->target_value;
                if (srcRegion != targetRegion) {
                    std::pair<int,int> pair{ srcRegion, targetRegion };
                    searchPairs[pair] = &(*record);
                }
            }
            else
                throw std::logic_error(
                                "MULTREGT record is based on region: "
                                +  record->region_name
                                + " which is not in the deck");
        }

        for (auto iter = searchPairs.begin(); iter != searchPairs.end(); ++iter) {
            const MULTREGTRecord * record = (*iter).second;
            std::pair<int,int> pair = (*iter).first;
            const std::string& keyword = record->region_name;
            if (m_searchMap.count(keyword) == 0)
                m_searchMap[keyword] = MULTREGTSearchMap();

            m_searchMap[keyword][pair] = record;
        }
    }


    void MULTREGTScanner::assertKeywordSupported( const DeckKeyword& deckKeyword, const std::string& /* defaultRegion */) {
        for (const auto& deckRecord : deckKeyword) {
            const auto& srcItem = deckRecord.getItem("SRC_REGION");
            const auto& targetItem = deckRecord.getItem("TARGET_REGION");
            auto nnc_behaviour = MULTREGT::NNCBehaviourFromString(deckRecord.getItem("NNC_MULT").get<std::string>(0));

            if (!srcItem.defaultApplied(0) && !targetItem.defaultApplied(0))
                if (srcItem.get<int>(0) == targetItem.get<int>(0))
                    throw std::invalid_argument("Sorry - MULTREGT applied internally to a region is not yet supported");

            if (nnc_behaviour == MULTREGT::NOAQUNNC)
                throw std::invalid_argument("Sorry - currently we do not support \'NOAQUNNC\' for MULTREGT.");

         }
    }



    void MULTREGTScanner::addKeyword( const Eclipse3DProperties& props, const DeckKeyword& deckKeyword , const std::string& defaultRegion) {
        assertKeywordSupported( deckKeyword , defaultRegion );

        for (const auto& deckRecord : deckKeyword) {
            std::vector<int> src_regions;
            std::vector<int> target_regions;
            std::string region_name = defaultRegion;

            const auto& srcItem = deckRecord.getItem("SRC_REGION");
            const auto& targetItem = deckRecord.getItem("TARGET_REGION");
            const auto& regionItem = deckRecord.getItem("REGION_DEF");

            double trans_mult = deckRecord.getItem("TRAN_MULT").get<double>(0);
            auto directions = FaceDir::FromMULTREGTString( deckRecord.getItem("DIRECTIONS").get<std::string>(0));
            auto nnc_behaviour = MULTREGT::NNCBehaviourFromString(deckRecord.getItem("NNC_MULT").get<std::string>(0));

            if (regionItem.defaultApplied(0)) {
                if (!m_records.empty())
                    region_name = m_records.back().region_name;
            } else
                region_name = MULTREGT::RegionNameFromDeckValue( regionItem.get<std::string>(0) );

            if (srcItem.defaultApplied(0) || srcItem.get<int>(0) < 0)
                src_regions = props.getRegions( region_name );
            else
                src_regions.push_back(srcItem.get<int>(0));

            if (targetItem.defaultApplied(0) || targetItem.get<int>(0) < 0)
                target_regions = props.getRegions(region_name);
            else
                target_regions.push_back(targetItem.get<int>(0));

            for (int src_region : src_regions) {
                for (int target_region : target_regions)
                    m_records.push_back({src_region, target_region, trans_mult, directions, nnc_behaviour, region_name});
            }
        }

    }


    /*
      This function will check the region values in globalIndex1 and
      globalIndex and see if they match the regionvalues specified in
      the deck. The function checks both directions:

      Assume the relevant MULTREGT record looks like:

         1  2   0.10  XYZ  ALL M /

      I.e. we are checking for the boundary between regions 1 and
      2. We assign the transmissibility multiplier to the correct face
      of the cell with value 1:

         -----------
         | 1  | 2  |   =>  MultTrans( i,j,k,FaceDir::XPlus ) *= 0.50
         -----------

         -----------
         | 2  | 1  |   =>  MultTrans( i+1,j,k,FaceDir::XMinus ) *= 0.50
         -----------

    */
    double MULTREGTScanner::getRegionMultiplier(size_t globalIndex1 , size_t globalIndex2, FaceDir::DirEnum faceDir) const {

        for (auto iter = m_searchMap.begin(); iter != m_searchMap.end(); iter++) {
            const Opm::GridProperty<int>& region = m_e3DProps.getIntGridProperty( (*iter).first );
            const MULTREGTSearchMap& map = (*iter).second;

            int regionId1 = region.iget(globalIndex1);
            int regionId2 = region.iget(globalIndex2);

            std::pair<int,int> pair{ regionId1, regionId2 };
            if (map.count(pair) != 1 || !(map.at(pair)->directions & faceDir)) {
                pair = std::pair<int,int>{regionId2 , regionId1};
                if (map.count(pair) != 1 || !(map.at(pair)->directions & faceDir))
                    continue;
            }
            const MULTREGTRecord* record = map.at(pair);

            bool applyMultiplier = true;
            int i1 = globalIndex1 % region.getNX();
            int i2 = globalIndex2 % region.getNX();
            int j1 = globalIndex1 / region.getNX() % region.getNY();
            int j2 = globalIndex2 / region.getNX() % region.getNY();

            if (record->nnc_behaviour == MULTREGT::NNC){
                applyMultiplier = true;
                if ((std::abs(i1-i2) == 0 && std::abs(j1-j2) == 1) || (std::abs(i1-i2) == 1 && std::abs(j1-j2) == 0))
                    applyMultiplier = false;
            }
            else if (record->nnc_behaviour == MULTREGT::NONNC){
                applyMultiplier = false;
                if ((std::abs(i1-i2) == 0 && std::abs(j1-j2) == 1) || (std::abs(i1-i2) == 1 && std::abs(j1-j2) == 0))
                    applyMultiplier = true;
            }

            if (applyMultiplier)
                return record->trans_mult;

        }
        return 1;
    }
}
