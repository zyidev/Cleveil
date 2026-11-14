#pragma once

// ============================================================================
//  iconprovider.h —— 程序图标提取与缓存
//  列表滚动时 data() 会被高频调用，所以同一程序只解析一次图标。
// ============================================================================

#include "core/appinfo.h"

#include <QHash>
#include <QIcon>

class QPixmap;

namespace un {

class IconProvider {
public:
    static IconProvider &instance();

    // 取一个程序对应的图标；解析不到时返回内置的占位图标。
    QIcon icon(const InstalledApp &app, int size = 32);

    // 清空缓存（刷新列表后调用）
    void clear();

private:
    IconProvider() = default;

    // 依次尝试 DisplayIcon → 卸载程序 exe → InstallLocation 目录
    QString resolveIconPath(const InstalledApp &app, int *index) const;

    // 程序没有图标时用的默认占位图（运行时用 QPainter 画，不依赖资源文件）
    QPixmap defaultPixmap(int size) const;

    QHash<QString, QIcon> m_cache;
};

} // namespace un
