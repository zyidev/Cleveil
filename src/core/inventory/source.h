#pragma once

#include "core/inventory/types.h"

namespace un {

class IInventorySource {
public:
    virtual ~IInventorySource() = default;
    virtual InventorySourceId id() const = 0;
    virtual QVector<InventoryRecord> scan(QVector<InventorySourceError> *errors) = 0;
};

} // namespace un
