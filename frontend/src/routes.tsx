import { createBrowserRouter } from "react-router";
import { Auth } from "./pages/Auth";
import { CameraStream } from "./pages/CameraStream";

export const router = createBrowserRouter([
  {
    path: "/",
    Component: Auth,
  },
  {
    path: "/camera",
    Component: CameraStream,
  },
]);
