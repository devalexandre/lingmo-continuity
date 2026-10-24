/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QJsonObject>

// We never open ports by ourselves: when ufw or firewalld is running,
// Settings shows which ports to open and the command for it.
namespace Firewall {
// {name: "ufw"|"firewalld"|"", ports: "24800-24802/tcp, 24801/udp", command: "sudo …"}
QJsonObject hint(bool principal);
}
