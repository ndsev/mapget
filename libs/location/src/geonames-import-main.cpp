#include "mapget/location/geonames-import.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "Usage: mapget-geonames-import <cities1000.txt> <geonames-cities1000.sqlite>\n";
        return 2;
    }

    try {
        auto stats = mapget::createGeonamesLocationDatabase(argv[1], argv[2]);
        std::cout << "Imported " << stats.rowsImported << " of " << stats.rowsRead
                  << " GeoNames rows into " << argv[2] << "\n";
        return 0;
    }
    catch (std::exception const& e) {
        std::cerr << "GeoNames import failed: " << e.what() << "\n";
        return 1;
    }
}
