import ".." as Skin
import "." as LibraryComponent
import Mixxx 1.0 as Mixxx
import Qt.labs.qmlmodels
import QtQml
import QtQuick
import QtQml.Models
import QtQuick.Layouts
import QtQuick.Controls 2.15
import "../Theme"

Rectangle {
    id: root

    required property var model

    color: Theme.darkGray

    LibraryComponent.Control {
        id: libraryControl

        onFocusWidgetChanged: {
            switch (focusWidget) {
            case Skin.FocusedWidgetControl.WidgetKind.LibraryView:
                view.forceActiveFocus();
                break;
            }
        }
        onLoadSelectedTrack: (group, play) => {
            view.loadSelectedTrack(group, play);
        }
        onLoadSelectedTrackIntoNextAvailableDeck: play => {
            view.loadSelectedTrackIntoNextAvailableDeck(play);
        }
        onMoveVertical: offset => {
            view.selectionModel.moveSelectionVertical(offset);
        }
    }
    HorizontalHeaderView {
        id: horizontalHeader

        property int sortingColumn: -1
        property var sortingOrder: Qt.Descending

        anchors.left: parent.left
        anchors.margins: 5
        anchors.right: parent.right
        anchors.top: parent.top
        syncView: view

        delegate: Item {
            id: column

            required property string display
            required property int index

            implicitHeight: columnName.contentHeight + 5
            implicitWidth: columnName.contentWidth + 5

            MouseArea {
                id: columnMouseHandler

                acceptedButtons: Qt.LeftButton
                anchors.fill: parent

                onClicked: {
                    if (horizontalHeader.sortingColumn == index) {
                        horizontalHeader.sortingOrder = horizontalHeader.sortingOrder == Qt.DescendingOrder ? Qt.AscendingOrder : Qt.DescendingOrder;
                    } else {
                        horizontalHeader.sortingColumn = index;
                        horizontalHeader.sortingOrder = Qt.AscendingOrder;
                    }
                    view.model.sort(horizontalHeader.sortingColumn, horizontalHeader.sortingOrder);
                }
            }
            Text {
                id: columnName

                anchors.fill: parent
                anchors.leftMargin: 15
                color: Theme.textColor
                elide: Text.ElideRight
                font.capitalization: Font.Capitalize
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Medium
                horizontalAlignment: Text.AlignLeft
                text: display
                verticalAlignment: Text.AlignVCenter
            }
            Item {
                anchors {
                    bottom: parent.bottom
                    left: parent.left
                    leftMargin: 5
                    top: parent.top
                }
                Label {
                    id: sortIndicator

                    anchors.centerIn: parent
                    color: "red"
                    elide: Text.ElideRight
                    font.bold: true
                    font.capitalization: Font.AllUppercase
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.buttonFontPixelSize
                    horizontalAlignment: Text.AlignRight
                    rotation: horizontalHeader.sortingOrder == Qt.AscendingOrder ? 90 : -90
                    text: "▶"
                    verticalAlignment: Text.AlignVCenter
                    visible: horizontalHeader.sortingColumn == index
                }
            }
            Rectangle {
                id: columnResizer

                color: Theme.darkGray2
                width: 1

                anchors {
                    bottom: parent.bottom
                    right: parent.right
                    top: parent.top
                }
                MouseArea {
                    id: columnResizeHandler

                    property int sizeOffset: 0

                    anchors.fill: parent
                    cursorShape: Qt.SizeHorCursor
                    preventStealing: true

                    onMouseXChanged: {
                        if (drag.active) {
                            column.width += mouseX;
                            sizeOffset += mouseX;
                        }
                    }

                    drag {
                        axis: Drag.XAxis
                        target: parent
                        threshold: 2

                        onActiveChanged: {
                            if (!drag.active && columnResizeHandler.sizeOffset !== 0) {
                                view.model.columns[index].preferredWidth = column.width;
                                columnResizeHandler.sizeOffset = 0;
                                view.updateColumnSize();
                                view.forceLayout();
                            }
                        }
                    }
                }
            }
        }
    }
    // Cell wrapper of the track table. An inline component, so the Android DelegateChooser below can make one
    // delegate per column from it.
    component TrackCell: Item {
        id: item

        required property int column
        required property url cover_art
        required property color decoration
        required property var display
        required property string file_url
        required property int row
        required property bool selected
        required property var track
        readonly property var tableView: TableView.view

        implicitHeight: 30

        Loader {
            id: loader

            property var capabilities: item.tableView && item.tableView.model ? item.tableView.model.getCapabilities() : Mixxx.LibraryTrackListModel.Capability.None
            property url cover_art: item.cover_art
            property color decoration: item.decoration
            property var display: item.display
            property url file_url: item.file_url
            property int row: item.row
            property bool selected: item.selected
            property var tableView: item.tableView
            property var track: item.track

            anchors.fill: parent
            focus: true
            sourceComponent: delegate

            onLoaded:
            // Workaround needed for WaveformOverview column to load the data
            //     if (track)
            //         Mixxx.Library.analyze(track)
            {}
        }
        // Workaround needed for WaveformOverview column to load the data
        // TableView.onReused: {
        //     if (track)
        //         Mixxx.Library.analyze(track)
        // }
    }
    TableView {
        id: view

        // Android: one delegate component per column. TableView only reuses a pooled item for a cell made from the
        // same delegate component, so a pooled wrapper never moves to another column, where its Loader would swap
        // sourceComponent and build a new Cell. Covers the 9 columns of SourceTree.qml defaultColumns, further
        // columns share the last choice.
        property Component androidCellChooser: DelegateChooser {
            DelegateChoice {
                column: 0

                TrackCell {
                }
            }
            DelegateChoice {
                column: 1

                TrackCell {
                }
            }
            DelegateChoice {
                column: 2

                TrackCell {
                }
            }
            DelegateChoice {
                column: 3

                TrackCell {
                }
            }
            DelegateChoice {
                column: 4

                TrackCell {
                }
            }
            DelegateChoice {
                column: 5

                TrackCell {
                }
            }
            DelegateChoice {
                column: 6

                TrackCell {
                }
            }
            DelegateChoice {
                column: 7

                TrackCell {
                }
            }
            DelegateChoice {
                column: 8

                TrackCell {
                }
            }
            DelegateChoice {
                TrackCell {
                }
            }
        }
        property int dynamicColumnCount: 0
        // Android: the TableView is not resized when the library area changes height (LIBRARY toggle). It keeps the
        // tallest height it got since the last width change, root clips it (Library.qml sets clip) and bottomMargin
        // keeps the last rows reachable. Each TableView resize drains the reuse pool and, while a leading column is
        // hidden (cover column autoHideWidth), forceLayout() reloads every visible cell (Qt 6.10
        // checkForVisibilityChanges()), which made every toggle rebuild the whole viewport.
        property real keptHeight: 0
        property Component trackCell: TrackCell {
        }
        property int usedWidth: 0
        readonly property real visibleHeight: Math.max(0, root.height - anchors.margins - y)

        function loadSelectedTrack(group, play) {
            const urls = this.selectionModel.selectedTrackUrls();
            if (urls.length == 0)
                return;

            Mixxx.PlayerManager.getPlayer(group).loadTrackFromLocationUrl(urls[0], play);
        }
        function loadSelectedTrackIntoNextAvailableDeck(play) {
            const urls = this.selectionModel.selectedTrackUrls();
            if (urls.length == 0)
                return;

            Mixxx.PlayerManager.loadLocationUrlIntoNextAvailableDeck(urls[0], play);
        }
        // Android: the view is taller than its visible part, so TableView.Contain treats rows below the clip as
        // visible. Scroll so that the row's bottom edge ends up inside the visible (clipped) area.
        function scrollRowIntoVisibleArea(row) {
            let item = null;
            for (let c = 0; c < columns && !item; c++) {
                if (columnWidthProvider(c) > 0)
                    item = itemAtCell(Qt.point(c, row));
            }
            if (!item)
                return;
            const rowTop = item.y;
            const rowBottom = item.y + item.height;
            const maxContentY = originY + Math.max(0, contentHeight - visibleHeight);
            let target = contentY;
            if (rowBottom > contentY + visibleHeight)
                target = rowBottom - visibleHeight;
            if (rowTop < target)
                target = rowTop;
            contentY = Math.max(originY, Math.min(maxContentY, target));
        }
        function updateColumnSize() {
            const oldUsedWidth = usedWidth;
            const oldDynamicColumnCount = dynamicColumnCount;
            usedWidth = 0;
            dynamicColumnCount = 0;
            if (model == null) {
                return;
            }
            for (let c = 0; c < model.columns.length; c++) {
                if (model.columns[c].hidden || model.columns[c].autoHideWidth > view.width) {
                    continue;
                } else if (model.columns[c].preferredWidth > 0) {
                    usedWidth += model.columns[c].preferredWidth;
                } else {
                    dynamicColumnCount += model.columns[c].fillSpan || 1;
                }
            }
            return oldDynamicColumnCount != dynamicColumnCount || oldUsedWidth != usedWidth;
        }

        anchors.bottom: Qt.platform.os === "android" ? undefined : parent.bottom
        anchors.left: parent.left
        anchors.margins: 5
        anchors.right: parent.right
        anchors.top: horizontalHeader.bottom
        clip: true
        columnWidthProvider: function (column) {
            const columnDef = view.model.columns[column];
            if (columnDef.hidden) {
                return 0;
            }
            if (columnDef.autoHideWidth > 0 && columnDef.autoHideWidth > view.width) {
                return 0;
            }
            if (columnDef.preferredWidth >= 0) {
                return columnDef.preferredWidth;
            }
            const span = columnDef.fillSpan || 1;
            return span * (view.width - view.usedWidth) / view.dynamicColumnCount;
        }
        keyNavigationEnabled: false
        model: root.model
        pointerNavigationEnabled: false
        reuseItems: true

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AlwaysOn
            // Android uses androidScrollBar, sized to the visible part of the view.
            visible: Qt.platform.os !== "android"
        }
        delegate: Qt.platform.os === "android" ? androidCellChooser : trackCell
        selectionModel: ItemSelectionModel {
            function moveSelectionVertical(value) {
                if (value == 0)
                    return;

                const selected = this.selectedIndexes;
                const oldRow = (selected.length == 0) ? 0 : selected[0].row;
                this.selectRow(oldRow + value);
            }
            function selectRow(row) {
                const rowCount = this.model.rowCount();
                if (rowCount == 0) {
                    this.clear();
                    return;
                }
                const newRow = Mixxx.MathUtils.positiveModulo(row, rowCount);
                this.select(this.model.index(newRow, 0), ItemSelectionModel.Rows | ItemSelectionModel.Select | ItemSelectionModel.Clear | ItemSelectionModel.Current);
                view.positionViewAtRow(newRow, TableView.Contain);
                if (Qt.platform.os === "android")
                    view.scrollRowIntoVisibleArea(newRow);
            }
            function selectedTrackUrls() {
                return this.selectedIndexes.map(index => {
                    return this.model.getUrl(index.row);
                });
            }

            model: view.model
        }

        Component.onCompleted: this.updateColumnSize()
        Keys.onDownPressed: this.selectionModel.moveSelectionVertical(1)
        Keys.onEnterPressed: this.loadSelectedTrackIntoNextAvailableDeck(false)
        Keys.onReturnPressed: this.loadSelectedTrackIntoNextAvailableDeck(false)
        Keys.onUpPressed: this.selectionModel.moveSelectionVertical(-1)
        onModelChanged: this.updateColumnSize()
        onVisibleHeightChanged: {
            if (Qt.platform.os !== "android")
                return;
            if (visibleHeight > keptHeight)
                keptHeight = visibleHeight;
            // The visible area grew while scrolled to the end: pull the last row back to the visible bottom.
            const maxContentY = originY + Math.max(0, contentHeight - visibleHeight);
            if (contentY > maxContentY)
                contentY = maxContentY;
        }
        onWidthChanged: {
            if (Qt.platform.os === "android") {
                // A width change re-lays out the table anyway (rotation, fold): drop the kept height there.
                keptHeight = visibleHeight;
            }
            if (view.updateColumnSize()) {
                // forceLayout is costly - only invoke if there was a change in the column layouts
                view.forceLayout();
            }
        }
        Binding {
            property: "height"
            target: view
            value: Math.max(view.keptHeight, view.visibleHeight)
            when: Qt.platform.os === "android"
        }
        Binding {
            property: "bottomMargin"
            target: view
            value: view.height - view.visibleHeight
            when: Qt.platform.os === "android"
        }
    }
    // Android: keeps the bottom margin strip empty now that the taller view reaches into it (root clips below).
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        color: root.color
        height: view.anchors.margins
        visible: Qt.platform.os === "android"
    }
    // Android: the view is taller than its visible part (see TableView.keptHeight), so the attached scroll bar
    // would be sized and positioned for the hidden part too.
    ScrollBar {
        id: androidScrollBar

        anchors.right: view.right
        anchors.top: view.top
        height: view.visibleHeight
        orientation: Qt.Vertical
        policy: ScrollBar.AlwaysOn
        position: view.contentHeight > 0 ? (view.contentY - view.originY) / view.contentHeight : 0
        size: view.contentHeight > 0 ? Math.min(1, view.visibleHeight / view.contentHeight) : 1
        visible: Qt.platform.os === "android"

        onPositionChanged: {
            if (pressed)
                view.contentY = view.originY + position * view.contentHeight;
        }
    }
}
