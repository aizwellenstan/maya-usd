//
// Copyright 2020 Autodesk
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "UsdTransform3dMayaXformStack.h"

#include <mayaUsd/ufe/Utils.h>
#include <mayaUsd/ufe/RotationUtils.h>
#include <mayaUsd/fileio/utils/xformStack.h>

#include "private/UfeNotifGuard.h"

#include <maya/MEulerRotation.h>

#include <map>

namespace {

using namespace MayaUsd::ufe;

// Type traits for GfVec precision.
template<class V>
struct OpPrecision {
    static UsdGeomXformOp::Precision precision;
};

template<>
UsdGeomXformOp::Precision OpPrecision<GfVec3f>::precision = 
    UsdGeomXformOp::PrecisionFloat;

template<>
UsdGeomXformOp::Precision OpPrecision<GfVec3d>::precision = 
    UsdGeomXformOp::PrecisionDouble;

VtValue getValue(const UsdAttribute& attr, const UsdTimeCode& time)
{
    VtValue value;
    attr.Get(&value, time);
    return value;
}

// UsdMayaXformStack::FindOpIndex() requires an inconvenient isInvertedTwin
// argument, various rotate transform op equivalences in a separate
// UsdMayaXformStack::IsCompatibleType().  Just roll our own op name to
// Maya transform stack index position.
const std::unordered_map<TfToken, UsdTransform3dMayaXformStack::OpNdx, TfToken::HashFunctor> gOpNameToNdx{
    {TfToken("xformOp:translate"),  UsdTransform3dMayaXformStack::NdxTranslate},
    {TfToken("xformOp:translate:rotatePivotTranslate"), UsdTransform3dMayaXformStack::NdxRotatePivotTranslate},
    {TfToken("xformOp:translate:rotatePivot"), UsdTransform3dMayaXformStack::NdxRotatePivot},
    {TfToken("xformOp:rotateX"),    UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:rotateY"),    UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:rotateZ"),    UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:rotateXYZ"),  UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:rotateXZY"),  UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:rotateYXZ"),  UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:rotateYZX"),  UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:rotateZXY"),  UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:rotateZYX"),  UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:orient"),     UsdTransform3dMayaXformStack::NdxRotate},
    {TfToken("xformOp:rotateXYZ:rotateAxis"), UsdTransform3dMayaXformStack::NdxRotateAxis},
    {TfToken("!invert!xformOp:translate:rotatePivot"), UsdTransform3dMayaXformStack::NdxRotatePivotInverse},
    {TfToken("xformOp:translate:scalePivotTranslate"), UsdTransform3dMayaXformStack::NdxScalePivotTranslate},
    {TfToken("xformOp:translate:scalePivot"), UsdTransform3dMayaXformStack::NdxScalePivot},
    {TfToken("xformOp:transform:shear"), UsdTransform3dMayaXformStack::NdxShear},
    {TfToken("xformOp:scale"),      UsdTransform3dMayaXformStack::NdxScale},
    {TfToken("!invert!xformOp:translate:scalePivot"), UsdTransform3dMayaXformStack::NdxScalePivotInverse}};

}

namespace MAYAUSD_NS_DEF {
namespace ufe {

namespace {

inline Ufe::Transform3d::Ptr nextTransform3d(
    const Ufe::Transform3dHandler::Ptr& nextHandler,
    const Ufe::SceneItem::Ptr&          item
) {
    return nextHandler->transform3d(item);
}

inline Ufe::Transform3d::Ptr nextEditTransform3d(
    const Ufe::Transform3dHandler::Ptr& nextHandler,
    const Ufe::SceneItem::Ptr&          item
) {
    return nextHandler->editTransform3d(item);
}

typedef Ufe::Transform3d::Ptr (*NextTransform3dFn)(
    const Ufe::Transform3dHandler::Ptr& nextHandler,
    const Ufe::SceneItem::Ptr&          item
);

Ufe::Transform3d::Ptr createTransform3d(
    const Ufe::Transform3dHandler::Ptr& nextHandler,
    const Ufe::SceneItem::Ptr&          item,
    NextTransform3dFn                   nextTransform3dFn
)
{
    UsdSceneItem::Ptr usdItem = std::dynamic_pointer_cast<UsdSceneItem>(item);
#if !defined(NDEBUG)
    assert(usdItem);
#endif

    // If the prim isn't transformable, can't create a Transform3d interface
    // for it.
    UsdGeomXformable xformSchema(usdItem->prim());
    if (!xformSchema) {
        return nullptr;
    }
    bool resetsXformStack = false;
    auto xformOps = xformSchema.GetOrderedXformOps(&resetsXformStack);

    // Early out: if there are no transform ops yet, it's a match.
    if (xformOps.empty()) {
        return UsdTransform3dMayaXformStack::create(
            usdItem, std::vector<UsdGeomXformOp>());
    }

    // If the prim supports the Maya transform stack, create a Maya transform
    // stack interface for it, otherwise delegate to the next handler in the
    // chain of responsibility.
    auto stackOps = UsdMayaXformStack::MayaStack().MatchingSubstack(xformOps);

    return stackOps.empty() ? nextTransform3dFn(nextHandler, item) :
        UsdTransform3dMayaXformStack::create(usdItem, xformOps);
}

// Helper class to factor out common code for translate, rotate, scale
// undoable commands.
class UsdTRSUndoableCmdBase : public Ufe::SetVector3dUndoableCommand {
private:
    UsdGeomXformOp    fOp;
    const UsdTimeCode fReadTime;
    const UsdTimeCode fWriteTime;
    const VtValue     fPrevOpValue;
    const TfToken     fAttrName;

public:
    UsdTRSUndoableCmdBase(
        const VtValue&        newOpValue,
        const Ufe::Path&      path,
        const UsdGeomXformOp& op,
        const UsdTimeCode&    writeTime_
    ) : Ufe::SetVector3dUndoableCommand(path), fOp(op),
        // Always read from proxy shape time.
        fReadTime(getTime(path)),
        fWriteTime(writeTime_),
        fPrevOpValue(getValue(op.GetAttr(), readTime())),
        fAttrName(op.GetOpName()),
        fNewOpValue(newOpValue)
    {}

    // Have separate execute override which is value-only, as default execute()
    // calls redo, and undo / redo will eventually not be value only and will
    // support propertySpec / primSpec management.
    void execute() override { setValue(fNewOpValue); }

    void recreateOp() {
        auto sceneItem = std::dynamic_pointer_cast<UsdSceneItem>(
            Ufe::Hierarchy::createItem(path())); TF_AXIOM(sceneItem);
        auto prim = sceneItem->prim(); TF_AXIOM(prim);
        auto attr = prim.GetAttribute(fAttrName); TF_AXIOM(attr);
        fOp       = UsdGeomXformOp(attr);
    }

    void undo() override {
        // Re-create the XformOp, as the underlying prim may have been
        // invalidated by later commands.
        recreateOp();
        setValue(fPrevOpValue);
    }
    void redo() override {
        // Re-create the XformOp, as the underlying prim may have been
        // invalidated by earlier commands.
        recreateOp();
        setValue(fNewOpValue);
    }

    void setValue(const VtValue& v) { fOp.GetAttr().Set(v, fWriteTime); }

    UsdTimeCode readTime() const { return fReadTime; }
    UsdTimeCode writeTime() const { return fWriteTime; }

protected:
    VtValue fNewOpValue;
};

// UsdRotatePivotTranslateUndoableCmd uses hard-coded USD common transform API
// single pivot attribute name, not reusable.
template<class V>
class UsdVecOpUndoableCmd : public UsdTRSUndoableCmdBase {
public:
    UsdVecOpUndoableCmd(
        const V&              v,
        const Ufe::Path&      path,
        const UsdGeomXformOp& op,
        const UsdTimeCode&    writeTime
    ) : UsdTRSUndoableCmdBase(VtValue(v), path, op, writeTime)
    {}

    // Executes the command by setting the translation onto the transform op.
    bool set(double x, double y, double z) override
    {
        fNewOpValue = V(x, y, z);

        setValue(fNewOpValue);
        return true;
    }
};

class UsdRotateOpUndoableCmd : public UsdTRSUndoableCmdBase {
public:
    UsdRotateOpUndoableCmd(
        const GfVec3f&        r,
        const Ufe::Path&      path,
        const UsdGeomXformOp& op,
        UsdTransform3dMayaXformStack::CvtRotXYZToAttrFn cvt,
        const UsdTimeCode&    writeTime
    ) : UsdTRSUndoableCmdBase(VtValue(r), path, op, writeTime), 
        _cvtRotXYZToAttr(cvt)
    {}

    // Executes the command by setting the rotation onto the transform op.
    bool set(double x, double y, double z) override
    {
        fNewOpValue = _cvtRotXYZToAttr(x, y, z);

        setValue(fNewOpValue);
        return true;
    }

private:

    // Convert from UFE RotXYZ rotation to a value for the transform op.
    UsdTransform3dMayaXformStack::CvtRotXYZToAttrFn _cvtRotXYZToAttr;
};

}

UsdTransform3dMayaXformStack::UsdTransform3dMayaXformStack(
    const UsdSceneItem::Ptr& item, const std::vector<UsdGeomXformOp>& ops
) : UsdTransform3dMayaXformStack(item, ops, gOpNameToNdx)
{}

UsdTransform3dMayaXformStack::UsdTransform3dMayaXformStack(
    const UsdSceneItem::Ptr&           item,
    const std::vector<UsdGeomXformOp>& ops,
    const OpNameToNdx&                 opNameToNdx
)
    : UsdTransform3dBase(item), _xformable(prim())
{
    TF_AXIOM(_xformable);

    // *** FIXME ***  We ask for ordered transform ops, but the prim may have
    // other transform ops that are not in the ordered list.  However, those
    // transform ops are not contributing to the final local transform.
    for (const auto& op : ops) {
        auto ndx = opNameToNdx.at(op.GetOpName());
        _orderedOps[ndx] = op;
    }
}

/* static */
UsdTransform3dMayaXformStack::Ptr UsdTransform3dMayaXformStack::create(
    const UsdSceneItem::Ptr& item, const std::vector<UsdGeomXformOp>& ops
)
{
    return std::make_shared<UsdTransform3dMayaXformStack>(item, ops);
}

Ufe::Vector3d UsdTransform3dMayaXformStack::translation() const
{
    return getVector3d<GfVec3d>(UsdGeomXformOp::GetOpName(
        UsdGeomXformOp::TypeTranslate, getTRSOpSuffix()));
}

Ufe::Vector3d UsdTransform3dMayaXformStack::rotation() const
{
    if (!hasOp(NdxRotate)) {
        return Ufe::Vector3d(0, 0, 0);
    }
    UsdGeomXformOp r = getOp(NdxRotate);
    TF_AXIOM(r);

    CvtRotXYZFromAttrFn cvt = getCvtRotXYZFromAttrFn(r.GetOpName());
    return cvt(getValue(r.GetAttr(), getTime(path())));
}

Ufe::Vector3d UsdTransform3dMayaXformStack::scale() const
{
    if (!hasOp(NdxScale)) {
        return Ufe::Vector3d(1, 1, 1);
    }
    UsdGeomXformOp s = getOp(NdxScale);
    TF_AXIOM(s);

    GfVec3f v;
    s.Get(&v, getTime(path()));
    return toUfe(v);
}

Ufe::TranslateUndoableCommand::Ptr UsdTransform3dMayaXformStack::translateCmd(double x, double y, double z)
{
    return setVector3dCmd(GfVec3d(x, y, z), UsdGeomXformOp::GetOpName(
        UsdGeomXformOp::TypeTranslate, getTRSOpSuffix()), getTRSOpSuffix());
}

Ufe::RotateUndoableCommand::Ptr UsdTransform3dMayaXformStack::rotateCmd(double x, double y, double z)
{
    // If there is no rotate transform op yet, create it.
    UsdGeomXformOp r;
    GfVec3f v(x, y, z);
    CvtRotXYZToAttrFn cvt = toXYZ; // No conversion is default.
    if (!hasOp(NdxRotate)) {
        // Use notification guard, otherwise will generate one notification for
        // the xform op add, and another for the reorder.
        InTransform3dChange guard(path());
        r = _xformable.AddRotateXYZOp(UsdGeomXformOp::PrecisionFloat, getTRSOpSuffix());
        if (!TF_VERIFY(r)) {
            return nullptr;
        }
        r.Set(v);
        setXformOpOrder();
    }
    else {
        r = getOp(NdxRotate);
        cvt = getCvtRotXYZToAttrFn(r.GetOpName());
    }

    return std::make_shared<UsdRotateOpUndoableCmd>(
        v, path(), r, cvt, UsdTimeCode::Default());
}

Ufe::ScaleUndoableCommand::Ptr UsdTransform3dMayaXformStack::scaleCmd(double x, double y, double z)
{
    // If there is no scale transform op yet, create it.
    UsdGeomXformOp s;
    GfVec3f v(x, y, z);
    if (!hasOp(NdxScale)) {
        InTransform3dChange guard(path());
        s = _xformable.AddScaleOp(UsdGeomXformOp::PrecisionFloat, getTRSOpSuffix());
        if (!TF_VERIFY(s)) {
            return nullptr;
        }
        s.Set(v);
        setXformOpOrder();
    }
    else {
        s = getOp(NdxScale);
    }

    return std::make_shared<UsdVecOpUndoableCmd<GfVec3f>>(
        v, path(), s, UsdTimeCode::Default());
}

Ufe::TranslateUndoableCommand::Ptr
UsdTransform3dMayaXformStack::rotatePivotCmd(double x, double y, double z)
{
    return pivotCmd(getOpSuffix(NdxRotatePivot), x, y, z);
}

Ufe::Vector3d UsdTransform3dMayaXformStack::rotatePivot() const
{
    return getVector3d<GfVec3f>(UsdGeomXformOp::GetOpName(
        UsdGeomXformOp::TypeTranslate, getOpSuffix(NdxRotatePivot)));
}

Ufe::TranslateUndoableCommand::Ptr
UsdTransform3dMayaXformStack::scalePivotCmd(double x, double y, double z)
{
    return pivotCmd(getOpSuffix(NdxScalePivot), x, y, z);
}

Ufe::Vector3d UsdTransform3dMayaXformStack::scalePivot() const
{
    return getVector3d<GfVec3f>(UsdGeomXformOp::GetOpName(
        UsdGeomXformOp::TypeTranslate, getOpSuffix(NdxScalePivot)));
}

Ufe::TranslateUndoableCommand::Ptr
UsdTransform3dMayaXformStack::translateRotatePivotCmd(
    double x, double y, double z)
{
    auto opSuffix = getOpSuffix(NdxRotatePivotTranslate);
    auto attrName = UsdGeomXformOp::GetOpName(
        UsdGeomXformOp::TypeTranslate, opSuffix);
    return setVector3dCmd(GfVec3f(x, y, z), attrName, opSuffix);
}

Ufe::Vector3d UsdTransform3dMayaXformStack::rotatePivotTranslation() const
{
    return getVector3d<GfVec3f>(UsdGeomXformOp::GetOpName(
        UsdGeomXformOp::TypeTranslate, getOpSuffix(NdxRotatePivotTranslate)));
}

Ufe::TranslateUndoableCommand::Ptr
UsdTransform3dMayaXformStack::translateScalePivotCmd(
    double x, double y, double z)
{
    auto opSuffix = getOpSuffix(NdxScalePivotTranslate);
    auto attrName = UsdGeomXformOp::GetOpName(
        UsdGeomXformOp::TypeTranslate, opSuffix);
    return setVector3dCmd(GfVec3f(x, y, z), attrName, opSuffix);
}

Ufe::Vector3d UsdTransform3dMayaXformStack::scalePivotTranslation() const
{
    return getVector3d<GfVec3f>(UsdGeomXformOp::GetOpName(
        UsdGeomXformOp::TypeTranslate, getOpSuffix(NdxScalePivotTranslate)));
}

template<class V>
Ufe::Vector3d UsdTransform3dMayaXformStack::getVector3d(
    const TfToken& attrName
) const
{
    // If the attribute doesn't exist yet, return a zero vector.
    auto attr = prim().GetAttribute(attrName);
    if (!attr) {
        return Ufe::Vector3d(0, 0, 0);
    }

    UsdGeomXformOp op(attr);
    TF_AXIOM(op);

    V v;
    op.Get(&v, getTime(path()));
    return toUfe(v);
}

template<class V>
Ufe::SetVector3dUndoableCommand::Ptr
UsdTransform3dMayaXformStack::setVector3dCmd(
    const V& v, const TfToken& attrName, const TfToken& opSuffix
)
{
    UsdGeomXformOp op;

    // If the attribute doesn't exist yet, create it.
    auto attr = prim().GetAttribute(attrName);
    if (!attr) {
        InTransform3dChange guard(path());
        op = _xformable.AddTranslateOp(OpPrecision<V>::precision, opSuffix);
        if (!TF_VERIFY(op)) {
            return nullptr;
        }
        op.Set(v);
        setXformOpOrder();
    }
    else {
        op = UsdGeomXformOp(attr);
        TF_AXIOM(op);
    }

    return std::make_shared<UsdVecOpUndoableCmd<V>>(
        v, path(), op, UsdTimeCode::Default());
}

Ufe::TranslateUndoableCommand::Ptr UsdTransform3dMayaXformStack::pivotCmd(
    const TfToken& pvtOpSuffix,
    double x, double y, double z
)
{
    auto pvtAttrName = UsdGeomXformOp::GetOpName(
        UsdGeomXformOp::TypeTranslate, pvtOpSuffix);
    UsdGeomXformOp p;

    // If the pivot attribute pair doesn't exist yet, create it.
    auto attr = prim().GetAttribute(pvtAttrName);
    GfVec3f v(x, y, z);
    if (!attr) {
        // Without a notification guard each operation (each transform op
        // addition, setting the attribute value, and setting the transform op
        // order) will notify.  Observers would see an object in an inconsistent
        // state, especially after pivot is added but before its inverse is
        // added --- this does not match the Maya transform stack.  Use of
        // SdfChangeBlock is discouraged when calling USD APIs above Sdf, so
        // use our own guard.
        InTransform3dChange guard(path());
        p = _xformable.AddTranslateOp(UsdGeomXformOp::PrecisionFloat,
                                     pvtOpSuffix);
        auto pInv = _xformable.AddTranslateOp(UsdGeomXformOp::PrecisionFloat,
            pvtOpSuffix, /* isInverseOp */ true);
        if (!TF_VERIFY(p && pInv)) {
            return nullptr;
        }
        p.Set(v);
        setXformOpOrder();
    }
    else {
        p = UsdGeomXformOp(attr);
        TF_AXIOM(p);
    }

    return std::make_shared<UsdVecOpUndoableCmd<GfVec3f>>(
        v, path(), p, UsdTimeCode::Default());
}

void UsdTransform3dMayaXformStack::setXformOpOrder()
{
    // Simply adding a transform op appends to the op order vector.  Therefore,
    // after addition, we must sort the ops to preserve Maya transform stack
    // ordering.  Use the Maya transform stack indices to add to a map, then
    // simply traverse the map to obtain the transform ops in order.
    std::map<int, UsdGeomXformOp> orderedOps;
    bool resetsXformStack = false;
    auto oldOrder = _xformable.GetOrderedXformOps(&resetsXformStack);
    for (const auto& op : oldOrder) {
        auto ndx = gOpNameToNdx.at(op.GetOpName());
        orderedOps[ndx] = op;
    }

    // Set the transform op order attribute, and rebuild our indexed cache.
    _orderedOps.clear();
    std::vector<UsdGeomXformOp> newOrder;
    newOrder.reserve(oldOrder.size());
    for (const auto& orderedOp : orderedOps) {
        const auto& op = orderedOp.second;
        newOrder.emplace_back(op);
        auto ndx = gOpNameToNdx.at(op.GetOpName());
        _orderedOps[ndx] = op;
    }

    _xformable.SetXformOpOrder(newOrder, resetsXformStack);
}

bool UsdTransform3dMayaXformStack::hasOp(OpNdx ndx) const
{
    return _orderedOps.find(ndx) != _orderedOps.end();
}

UsdGeomXformOp UsdTransform3dMayaXformStack::getOp(OpNdx ndx) const
{
    return _orderedOps.at(ndx);
}

TfToken UsdTransform3dMayaXformStack::getOpSuffix(OpNdx ndx) const
{
    static std::unordered_map<OpNdx, TfToken> opSuffix = {
    {NdxRotatePivotTranslate, UsdMayaXformStackTokens->rotatePivotTranslate},
    {NdxRotatePivot, UsdMayaXformStackTokens->rotatePivot},
    {NdxRotateAxis, UsdMayaXformStackTokens->rotateAxis},
    {NdxScalePivotTranslate, UsdMayaXformStackTokens->scalePivotTranslate},
    {NdxScalePivot, UsdMayaXformStackTokens->scalePivot},
    {NdxShear, UsdMayaXformStackTokens->shear}
    };
    return opSuffix.at(ndx);
}

TfToken UsdTransform3dMayaXformStack::getTRSOpSuffix() const
{
    return TfToken();
}

UsdTransform3dMayaXformStack::CvtRotXYZFromAttrFn
UsdTransform3dMayaXformStack::getCvtRotXYZFromAttrFn(const TfToken& opName) const
{
    static std::unordered_map<TfToken, CvtRotXYZFromAttrFn, TfToken::HashFunctor> cvt = {
        {TfToken("xformOp:rotateX"),   fromX},
        {TfToken("xformOp:rotateY"),   fromY},
        {TfToken("xformOp:rotateZ"),   fromZ},
        {TfToken("xformOp:rotateXYZ"), fromXYZ},
        {TfToken("xformOp:rotateXZY"), fromXZY},
        {TfToken("xformOp:rotateYXZ"), fromYXZ},
        {TfToken("xformOp:rotateYZX"), fromYZX},
        {TfToken("xformOp:rotateZXY"), fromZXY},
        {TfToken("xformOp:rotateZYX"), fromZYX},
        {TfToken("xformOp:orient"),    nullptr}}; // FIXME, unsupported.

    return cvt.at(opName);
}

UsdTransform3dMayaXformStack::CvtRotXYZToAttrFn
UsdTransform3dMayaXformStack::getCvtRotXYZToAttrFn(const TfToken& opName) const
{
    static std::unordered_map<TfToken, CvtRotXYZToAttrFn, TfToken::HashFunctor> cvt = {
        {TfToken("xformOp:rotateX"),   toX},
        {TfToken("xformOp:rotateY"),   toY},
        {TfToken("xformOp:rotateZ"),   toZ},
        {TfToken("xformOp:rotateXYZ"), toXYZ},
        {TfToken("xformOp:rotateXZY"), toXZY},
        {TfToken("xformOp:rotateYXZ"), toYXZ},
        {TfToken("xformOp:rotateYZX"), toYZX},
        {TfToken("xformOp:rotateZXY"), toZXY},
        {TfToken("xformOp:rotateZYX"), toZYX},
        {TfToken("xformOp:orient"),    nullptr}}; // FIXME, unsupported.

    return cvt.at(opName);
}

//------------------------------------------------------------------------------
// UsdTransform3dMayaXformStackHandler
//------------------------------------------------------------------------------

UsdTransform3dMayaXformStackHandler::UsdTransform3dMayaXformStackHandler(
    const Ufe::Transform3dHandler::Ptr& nextHandler
) : Ufe::Transform3dHandler(), _nextHandler(nextHandler)
{}

/*static*/
UsdTransform3dMayaXformStackHandler::Ptr UsdTransform3dMayaXformStackHandler::create(
    const Ufe::Transform3dHandler::Ptr& nextHandler
)
{
    return std::make_shared<UsdTransform3dMayaXformStackHandler>(nextHandler);
}

Ufe::Transform3d::Ptr UsdTransform3dMayaXformStackHandler::transform3d(
    const Ufe::SceneItem::Ptr& item
) const
{
    return createTransform3d(_nextHandler, item, nextTransform3d);
}

Ufe::Transform3d::Ptr UsdTransform3dMayaXformStackHandler::editTransform3d(
    const Ufe::SceneItem::Ptr& item
) const
{
    return createTransform3d(_nextHandler, item, nextEditTransform3d);
}

} // namespace ufe
} // namespace MayaUsd
