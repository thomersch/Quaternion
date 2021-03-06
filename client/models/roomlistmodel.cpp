/**************************************************************************
 *                                                                        *
 * Copyright (C) 2016 Felix Rohrbach <kde@fxrh.de>                        *
 *                                                                        *
 * This program is free software; you can redistribute it and/or          *
 * modify it under the terms of the GNU General Public License            *
 * as published by the Free Software Foundation; either version 3         *
 * of the License, or (at your option) any later version.                 *
 *                                                                        *
 * This program is distributed in the hope that it will be useful,        *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 * GNU General Public License for more details.                           *
 *                                                                        *
 * You should have received a copy of the GNU General Public License      *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  *
 *                                                                        *
 **************************************************************************/

#include "roomlistmodel.h"

#include "../quaternionroom.h"

#include "lib/user.h"
#include "lib/connection.h"

#include <QtGui/QIcon>

RoomListModel::RoomListModel(QObject* parent)
    : QAbstractListModel(parent)
{ }

void RoomListModel::addConnection(Connection* connection)
{
    Q_ASSERT(connection);

    using QMatrixClient::Room;
    beginResetModel();
    m_connections.push_back(connection);
    connect( connection, &Connection::loggedOut,
             this, [=]{ deleteConnection(connection); } );
    connect( connection, &Connection::invitedRoom,
             this, &RoomListModel::updateRoom);
    connect( connection, &Connection::joinedRoom,
             this, &RoomListModel::updateRoom);
    connect( connection, &Connection::leftRoom,
             this, &RoomListModel::updateRoom);
    connect( connection, &Connection::aboutToDeleteRoom,
             this, &RoomListModel::deleteRoom);

    for( auto r: connection->roomMap() )
        doAddRoom(r);
    endResetModel();
}

void RoomListModel::deleteConnection(Connection* connection)
{
    Q_ASSERT(connection);

    // TODO: Save selection
    beginResetModel();
    connection->disconnect(this);
    for( QuaternionRoom* room: m_rooms )
        room->disconnect( this );
    m_rooms.erase(
        std::remove_if(m_rooms.begin(), m_rooms.end(),
            [=](const QuaternionRoom* r) { return r->connection() == connection; }),
        m_rooms.end());
    m_connections.removeOne(connection);
    endResetModel();
    // TODO: Restore selection
}

QuaternionRoom* RoomListModel::roomAt(QModelIndex index) const
{
    return m_rooms.at(index.row());
}

QModelIndex RoomListModel::indexOf(QuaternionRoom* room) const
{
    return index(m_rooms.indexOf(room), 0);
}

void RoomListModel::updateRoom(QMatrixClient::Room* room,
                               QMatrixClient::Room* prev)
{
    // There are two cases when this method is called:
    // 1. (prev == nullptr) adding a new room to the room list
    // 2. (prev != nullptr) accepting/rejecting an invitation or inviting to
    //    the previously left room (in both cases prev has the previous state).
    if (prev == room)
    {
        qCritical() << "RoomListModel::updateRoom: room tried to replace itself";
        refresh(static_cast<QuaternionRoom*>(room));
        return;
    }
    if (prev && room->id() != prev->id())
    {
        qCritical() << "RoomListModel::updateRoom: attempt to update room"
                    << room->id() << "to" << prev->id();
        // That doesn't look right but technically we still can do it.
    }
    // Ok, we're through with pre-checks, now for the real thing.
    auto* newRoom = static_cast<QuaternionRoom*>(room);
    const auto it = std::find_if(m_rooms.begin(), m_rooms.end(),
          [=](const QuaternionRoom* r) { return r == prev || r == newRoom; });
    if (it != m_rooms.end())
    {
        const int row = it - m_rooms.begin();
        // There's no guarantee that prev != newRoom
        if (*it == prev && *it != newRoom)
        {
            prev->disconnect(this);
            m_rooms.replace(row, newRoom);
            connectRoomSignals(newRoom);
        }
        emit dataChanged(index(row), index(row));
    }
    else
    {
        beginInsertRows(QModelIndex(), m_rooms.count(), m_rooms.count());
        doAddRoom(newRoom);
        endInsertRows();
    }
}

void RoomListModel::deleteRoom(QMatrixClient::Room* room)
{
    auto i = m_rooms.indexOf(static_cast<QuaternionRoom*>(room));
    if (i == -1)
        return; // Already deleted, nothing to do

    beginRemoveRows(QModelIndex(), i, i);
    m_rooms.removeAt(i);
    endRemoveRows();
}

void RoomListModel::doAddRoom(QMatrixClient::Room* r)
{
    if (auto* room = static_cast<QuaternionRoom*>(r))
    {
        m_rooms.append(room);
        connectRoomSignals(room);
    } else
    {
        qCritical() << "Attempt to add nullptr to the room list";
        Q_ASSERT(false);
    }
}

void RoomListModel::connectRoomSignals(QuaternionRoom* room)
{
    connect(room, &QuaternionRoom::displaynameChanged,
            this, [=]{ displaynameChanged(room); } );
    connect( room, &QuaternionRoom::unreadMessagesChanged,
             this, [=]{ unreadMessagesChanged(room); } );
    connect( room, &QuaternionRoom::notificationCountChanged,
             this, [=]{ unreadMessagesChanged(room); } );
    connect( room, &QuaternionRoom::joinStateChanged,
             this, [=]{ refresh(room); });
    connect( room, &QuaternionRoom::avatarChanged,
             this, [=]{ refresh(room, { Qt::DecorationRole }); });
}

int RoomListModel::rowCount(const QModelIndex& parent) const
{
    if( parent.isValid() )
        return 0;
    return m_rooms.count();
}

QVariant RoomListModel::data(const QModelIndex& index, int role) const
{
    if( !index.isValid() )
        return QVariant();

    if( index.row() >= m_rooms.count() )
    {
        qDebug() << "UserListModel: something wrong here...";
        return QVariant();
    }
    auto room = m_rooms.at(index.row());
    using QMatrixClient::JoinState;
    switch (role)
    {
        case Qt::DisplayRole:
        {
            const auto unreadCount = room->unreadCount();
            const auto postfix = unreadCount == -1 ? QString() :
                room->readMarker() != room->timelineEdge()
                    ? QStringLiteral(" [%1]").arg(unreadCount)
                    : QStringLiteral(" [%1+]").arg(unreadCount);
            for (auto c: m_connections)
            {
                if (c == room->connection())
                    continue;
                if (c->room(room->id(), room->joinState()))
                    return tr("%1 (as %2)").arg(room->displayName(),
                                                room->connection()->userId())
                           + postfix;
            }
            return room->displayName() + postfix;
        }
        case Qt::DecorationRole:
        {
            auto avatar = room->avatar(16, 16);
            if (!avatar.isNull())
                return avatar;
            switch( room->joinState() )
            {
                case JoinState::Join:
                    return QIcon(":/irc-channel-joined.svg");
                case JoinState::Invite:
                    return QIcon(":/irc-channel-invited.svg");
                case JoinState::Leave:
                    return QIcon(":/irc-channel-parted.svg");
            }
        }
        case Qt::ToolTipRole:
        {
            auto result = QStringLiteral("<b>%1</b><br>").arg(room->displayName());
            result += tr("Main alias: %1<br>").arg(room->canonicalAlias());
            result += tr("Members: %1<br>").arg(room->memberCount());

            auto directChatUsers = room->directChatUsers();
            if (!directChatUsers.isEmpty())
            {
                QStringList userNames;
                for (auto* user: directChatUsers)
                    userNames.push_back(user->displayname(room));
                result += tr("Direct chat with %1<br>")
                            .arg(userNames.join(','));
            }

            if (room->usesEncryption())
                result += tr("The room enforces encryption<br>");

            auto unreadCount = room->unreadCount();
            if (unreadCount >= 0)
            {
                const auto unreadLine =
                    room->readMarker() == room->timelineEdge()
                        ? tr("Unread messages: %1+<br>")
                        : tr("Unread messages: %1<br>");
                result += unreadLine.arg(unreadCount);
            }

            auto hlCount = room->highlightCount();
            if (hlCount > 0)
                result += tr("Unread highlights: %1<br>").arg(hlCount);

            result += tr("ID: %1<br>").arg(room->id());
            switch (room->joinState())
            {
                case JoinState::Join:
                    result += tr("You joined this room");
                    break;
                case JoinState::Leave:
                    result += tr("You left this room");
                    break;
                default:
                    result += tr("You were invited into this room");
            }
            return result;
        }
        case HasUnreadRole:
            return room->hasUnreadMessages();
        case HighlightCountRole:
            return room->highlightCount();
        case JoinStateRole:
            return toCString(room->joinState()); // FIXME: better make the enum QVariant-convertible (only possible from Qt 5.8, see Q_ENUM_NS)
        default:
            return QVariant();
    }
}

void RoomListModel::displaynameChanged(QuaternionRoom* room)
{
    refresh(room);
}

void RoomListModel::unreadMessagesChanged(QuaternionRoom* room)
{
    refresh(room);
}

void RoomListModel::refresh(QuaternionRoom* room, const QVector<int>& roles)
{
    int row = m_rooms.indexOf(room);
    if (row == -1)
        qCritical() << "Room" << room->id() << "not found in the room list";
    else
        emit dataChanged(index(row), index(row), roles);
}

