#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlField>
#include <QFileInfo>
#include <QPainter>
#include <QPixmapCache>
#include "common/rectc.h"
#include "common/wgs84.h"
#include "osm.h"
#include "config.h"
#include "mbtilesmap.h"


#define META_TYPE(type) static_cast<QMetaType::Type>(type)

static double index2mercator(int index, int zoom)
{
	return rad2deg(-M_PI + 2 * M_PI * ((double)index / (1<<zoom)));
}

MBTilesMap::MBTilesMap(const QString &fileName, QObject *parent)
  : Map(parent), _fileName(fileName), _deviceRatio(1.0), _tileRatio(1.0),
  _valid(false)
{
	_db = QSqlDatabase::addDatabase("QSQLITE", fileName);
	_db.setDatabaseName(fileName);

	if (!_db.open()) {
		_errorString = fileName + ": Error opening database file";
		return;
	}

	QSqlRecord r = _db.record("tiles");
	if (r.isEmpty()
	  || r.field(0).name() != "zoom_level"
	  || META_TYPE(r.field(0).type()) != QMetaType::Int
	  || r.field(1).name() != "tile_column"
	  || META_TYPE(r.field(1).type()) != QMetaType::Int
	  || r.field(2).name() != "tile_row"
	  || META_TYPE(r.field(2).type()) != QMetaType::Int
	  || r.field(3).name() != "tile_data"
	  || META_TYPE(r.field(3).type()) != QMetaType::QByteArray) {
		_errorString = "Invalid table format";
		return;
	}

	{
		QSqlQuery query("SELECT min(zoom_level), max(zoom_level) FROM tiles",
		  _db);
		if (!query.first()) {
			_errorString = "Empty tile set";
			return;
		}
		_zooms = Range(query.value(0).toInt(), query.value(1).toInt());
		if (_zooms.min() < 0 || !_zooms.isValid()) {
			_errorString = "Invalid zoom levels";
			return;
		}
	}
	_zoom = _zooms.max();

	{
		QString sql = QString("SELECT min(tile_column), min(tile_row), "
		  "max(tile_column), max(tile_row) FROM tiles WHERE zoom_level = %1")
		  .arg(_zooms.min());
		QSqlQuery query(sql, _db);
		query.first();

		double minX = index2mercator(qMin((1<<_zooms.min()) - 1,
		  qMax(0, query.value(0).toInt())), _zooms.min());
		double minY = index2mercator(qMin((1<<_zooms.min()) - 1,
		  qMax(0, query.value(1).toInt())), _zooms.min());
		double maxX = index2mercator(qMin((1<<_zooms.min()) - 1,
		  qMax(0, query.value(2).toInt())) + 1, _zooms.min());
		double maxY = index2mercator(qMin((1<<_zooms.min()) - 1,
		  qMax(0, query.value(3).toInt())) + 1, _zooms.min());
		Coordinates tl(osm::m2ll(QPointF(minX, maxY)));
		Coordinates br(osm::m2ll(QPointF(maxX, minY)));
		// Workaround of broken zoom level 0 and 1 due to numerical instability
		tl.rlat() = qMin(tl.lat(), 85.0511);
		br.rlat() = qMax(br.lat(), -85.0511);
		_bounds = RectC(tl, br);
	}

	_db.close();

	_valid = true;
}

QString MBTilesMap::name() const
{
	return QFileInfo(_fileName).fileName();;
}

void MBTilesMap::load()
{
	_db.open();
}

void MBTilesMap::unload()
{
	_db.close();
}

QRectF MBTilesMap::bounds()
{
	return QRectF(ll2xy(_bounds.topLeft()), ll2xy(_bounds.bottomRight()));
}

int MBTilesMap::limitZoom(int zoom) const
{
	if (zoom < _zooms.min())
		return _zooms.min();
	if (zoom > _zooms.max())
		return _zooms.max();

	return zoom;
}

int MBTilesMap::zoomFit(const QSize &size, const RectC &rect)
{
	if (!rect.isValid())
		_zoom = _zooms.max();
	else {
		QRectF tbr(osm::ll2m(rect.topLeft()), osm::ll2m(rect.bottomRight()));
		QPointF sc(tbr.width() / size.width(), tbr.height() / size.height());
		_zoom = limitZoom(osm::scale2zoom(qMax(sc.x(), -sc.y())
		  / coordinatesRatio()));
	}

	return _zoom;
}

qreal MBTilesMap::resolution(const QRectF &rect)
{
	qreal scale = osm::zoom2scale(_zoom);

	return (WGS84_RADIUS * 2.0 * M_PI * scale / 360.0
	  * cos(2.0 * atan(exp(deg2rad(-rect.center().y() * scale))) - M_PI/2));
}

int MBTilesMap::zoomIn()
{
	_zoom = qMin(_zoom + 1, _zooms.max());
	return _zoom;
}

int MBTilesMap::zoomOut()
{
	_zoom = qMax(_zoom - 1, _zooms.min());
	return _zoom;
}

qreal MBTilesMap::coordinatesRatio() const
{
	return _deviceRatio > 1.0 ? _deviceRatio / _tileRatio : 1.0;
}

qreal MBTilesMap::imageRatio() const
{
	return _deviceRatio > 1.0 ? _deviceRatio : _tileRatio;
}

qreal MBTilesMap::tileSize() const
{
	return (TILE_SIZE / coordinatesRatio());
}

QByteArray MBTilesMap::tileData(int zoom, const QPoint &tile) const
{
	QSqlQuery query(_db);
	query.prepare("SELECT tile_data FROM tiles "
	  "WHERE zoom_level=:zoom AND tile_column=:x AND tile_row=:y");
	query.bindValue(":zoom", zoom);
	query.bindValue(":x", tile.x());
	query.bindValue(":y", (1<<zoom) - tile.y() - 1);
	query.exec();

	if (query.first())
		return query.value(0).toByteArray();

	return QByteArray();
}

void MBTilesMap::draw(QPainter *painter, const QRectF &rect, Flags flags)
{
	Q_UNUSED(flags);
	qreal scale = osm::zoom2scale(_zoom);
	QRectF b(bounds());


	QPoint tile = osm::mercator2tile(QPointF(rect.topLeft().x() * scale,
	  -rect.topLeft().y() * scale) * coordinatesRatio(), _zoom);
	QPointF tl(floor(rect.left() / tileSize())
	  * tileSize(), floor(rect.top() / tileSize()) * tileSize());

	QSizeF s(qMin(rect.right() - tl.x(), b.width()),
	  qMin(rect.bottom() - tl.y(), b.height()));
	for (int i = 0; i < ceil(s.width() / tileSize()); i++) {
		for (int j = 0; j < ceil(s.height() / tileSize()); j++) {
			QPixmap pm;
			QPoint t(tile.x() + i, tile.y() + j);
			QString key = _fileName + "-" + QString::number(_zoom) + "_"
			  + QString::number(t.x()) + "_" + QString::number(t.y());

			if (!QPixmapCache::find(key, &pm))
				if (pm.loadFromData(tileData(_zoom, t)))
					QPixmapCache::insert(key, pm);

			QPointF tp(qMax(tl.x(), b.left()) + (t.x() - tile.x()) * tileSize(),
			  qMax(tl.y(), b.top()) + (t.y() - tile.y()) * tileSize());
			if (!pm.isNull()) {
#ifdef ENABLE_HIDPI
				pm.setDevicePixelRatio(imageRatio());
#endif // ENABLE_HIDPI
				painter->drawPixmap(tp, pm);
			}
		}
	}
}

QPointF MBTilesMap::ll2xy(const Coordinates &c)
{
	qreal scale = osm::zoom2scale(_zoom);
	QPointF m = osm::ll2m(c);
	return QPointF(m.x() / scale, m.y() / -scale) / coordinatesRatio();
}

Coordinates MBTilesMap::xy2ll(const QPointF &p)
{
	qreal scale = osm::zoom2scale(_zoom);
	return osm::m2ll(QPointF(p.x() * scale, -p.y() * scale)
	  * coordinatesRatio());
}
