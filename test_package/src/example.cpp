#include "mapget/model/info.h"
#include "mapget/service/service.h"
#include "mapget/http-service/http-client.h"
#include "mapget/http-datasource/datasource-client.h"

int main() {
    mapget::Version v;
    (void)v.toJson();

    return 0;
}
