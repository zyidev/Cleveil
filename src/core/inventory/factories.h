#pragma once

#include "core/inventory/source.h"

namespace un {

IInventorySource *createArpInventorySource();
IInventorySource *createMsiInventorySource();
IInventorySource *createAppxInventorySource();
IInventorySource *createDriverInventorySource();
IInventorySource *createServiceInventorySource();
IInventorySource *createInstallerFileInventorySource();
IInventorySource *createShortcutInventorySource();
IInventorySource *createExeVersionInventorySource();
IInventorySource *createSnapshotDiffInventorySource();

} // namespace un
