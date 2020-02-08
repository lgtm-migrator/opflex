package org.opendaylight.opflex.genie.content.format.agent.build.automake.cpp;

import org.opendaylight.opflex.genie.engine.format.*;
import org.opendaylight.opflex.genie.engine.proc.Config;

/**
 * Created by midvorki on 10/7/14.
 */
public class FLibPcIn
        extends GenericFormatterTask
{
    public FLibPcIn(
        FormatterCtx aInFormatterCtx,
        FileNameRule aInFileNameRule,
        Indenter aInIndenter,
        BlockFormatDirective aInHeaderFormatDirective,
        BlockFormatDirective aInCommentFormatDirective,
        boolean aInIsUserFile)
    {
        super(aInFormatterCtx,
              aInFileNameRule,
              aInIndenter,
              aInHeaderFormatDirective,
              aInCommentFormatDirective,
              aInIsUserFile
        );
    }

    /**
     * Optional API required by the framework for transformation of file naming rule for the corresponding
     * generated file. This method can customize the location for the generated file.
     * @param aInFnr file name rule template
     * @return transformed file name rule
     */
    public static FileNameRule transformFileNameRule(FileNameRule aInFnr)
    {
        String lLibName = Config.getLibName();
        FileNameRule lFnr = new FileNameRule(
                aInFnr.getRelativePath(),
                null,
                aInFnr.getFilePrefix(),
                aInFnr.getFileSuffix(),
                aInFnr.getFileExtension(),
                lLibName);

        return lFnr;
    }

    public void firstLineCb()
    {
        out.println("#!/bin/sh");
    }

    public void generate()
    {
        String lModuleName = Config.getProjName();
        out.println(FORMAT.replaceAll("_MODULE_NAME_", lModuleName));
    }

    public static final String FORMAT = "prefix=@prefix@\n" + "exec_prefix=@exec_prefix@\n" + "libdir=@libdir@\n"
                                        + "includedir=@includedir@\n" + "\n" + "Name: @PACKAGE@\n"
                                        + "Description: OpFlex Framework Generated Model\n" + "Version: @VERSION@\n"
                                        + "Libs: -L${libdir} -l_MODULE_NAME_\n" + "Libs.private: @LIBS@\n"
                                        + "Cflags: -I${includedir} @BOOST_CPPFLAGS@\n";

}
