import java.io.*;
import java.util.zip.*;
import javax.xml.XMLConstants;
import javax.xml.transform.stream.StreamSource;
import javax.xml.validation.*;

public class Validate {
  public static void main(String[] args) throws Exception {
    SchemaFactory f = SchemaFactory.newInstance(XMLConstants.W3C_XML_SCHEMA_NS_URI);
    Schema project = f.newSchema(new File(args[0]));
    Schema meta = f.newSchema(new File(args[1]));
    int bad = 0;
    for (int i = 2; i < args.length; i++) {
      try (ZipFile z = new ZipFile(args[i])) {
        for (String[] e : new String[][] {{"project.xml", "p"}, {"metadata.xml", "m"}}) {
          ZipEntry en = z.getEntry(e[0]);
          if (en == null) { System.out.println(args[i] + ": missing " + e[0]); bad++; continue; }
          try {
            (e[1].equals("p") ? project : meta).newValidator().validate(new StreamSource(z.getInputStream(en)));
          } catch (org.xml.sax.SAXParseException ex) {
            System.out.println(args[i] + " " + e[0] + ":" + ex.getLineNumber() + ": " + ex.getMessage()); bad++;
          }
        }
        // every referenced embedded file must exist
        String xml = new String(z.getInputStream(z.getEntry("project.xml")).readAllBytes(), "UTF-8");
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("<(?:File|State) path=\"([^\"]*)\"( external=\"true\")?").matcher(xml);
        while (m.find()) if (m.group(2) == null && z.getEntry(m.group(1)) == null) { System.out.println(args[i] + ": missing entry " + m.group(1)); bad++; }
      }
    }
    System.out.println(bad == 0 ? "valid" : bad + " problem(s)");
    System.exit(bad == 0 ? 0 : 1);
  }
}
