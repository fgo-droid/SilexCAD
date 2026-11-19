#include "kernel/StepIO.h"

#include <sstream>
#include <stdexcept>

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>

namespace silex::kernel {

TopoDS_Shape readStep(const std::string& path)
{
    STEPControl_Reader reader;
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone)
        throw std::runtime_error("Cannot read STEP file: " + path);
    if (reader.TransferRoots() == 0)
        throw std::runtime_error("The STEP file contains no transferable shape");
    TopoDS_Shape s = reader.OneShape();
    if (s.IsNull())
        throw std::runtime_error("The STEP file contains no geometry");
    return s;
}

void writeStep(const TopoDS_Shape& shape, const std::string& path)
{
    STEPControl_Writer writer;
    Interface_Static::SetCVal("write.step.schema", "AP214IS");
    Interface_Static::SetCVal("write.step.unit", "MM");
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone)
        throw std::runtime_error("STEP translation failed");
    if (writer.Write(path.c_str()) != IFSelect_RetDone)
        throw std::runtime_error("Cannot write STEP file: " + path);
}

std::string shapeToBrep(const TopoDS_Shape& s)
{
    std::ostringstream os;
    BRepTools::Write(s, os);
    return os.str();
}

TopoDS_Shape brepToShape(const std::string& brep)
{
    std::istringstream is(brep);
    TopoDS_Shape s;
    BRep_Builder b;
    BRepTools::Read(s, is, b);
    return s;
}

}  // namespace silex::kernel
