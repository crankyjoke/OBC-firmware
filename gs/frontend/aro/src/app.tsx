import "./app.css";
import { BrowserRouter, Route, Routes } from "react-router-dom";
import Home from "./home/home.tsx";
import Navbar from "./common/navbar.tsx";
import NotFound from "./common/not_found.tsx";
import NewRequestForm from "./new_request/new_request_form.tsx";
import Requests from "./requests/requests.tsx";
import Profile from "./profile/profile.tsx";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";

const queryClient = new QueryClient();

function App() {
  return (
    <QueryClientProvider client={queryClient}>
      <BrowserRouter>
        <Navbar />
        <Routes>
          <Route path="*" element={<NotFound />} />
          <Route path="/" element={<Home />} />
          <Route path="/new-request" element={<NewRequestForm />} />
          <Route path="/requests" element={<Requests />} />
          <Route path="/profile" element={<Profile />} />
        </Routes>
      </BrowserRouter>
    </QueryClientProvider>
  );
}

export default App;
